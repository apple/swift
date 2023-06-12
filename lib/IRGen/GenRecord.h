//===--- GenRecord.h - IR generation for record types -----------*- C++ -*-===//
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
//  This file provides some common code for emitting record types.
//  A record type is something like a tuple or a struct.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_GENRECORD_H
#define SWIFT_IRGEN_GENRECORD_H

#include "BitPatternBuilder.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "Explosion.h"
#include "GenEnum.h"
#include "GenOpaque.h"
#include "LoadableTypeInfo.h"
#include "Outlining.h"
#include "TypeInfo.h"
#include "StructLayout.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/TrailingObjects.h"
#include "swift/AST/DiagnosticsIRGen.h"

namespace swift {
namespace irgen {

template <class, class, class> class RecordTypeBuilder;

/// A field of a record type.
template <class FieldImpl> class RecordField {
  ElementLayout Layout;

  template <class, class, class> friend class RecordTypeBuilder;

  /// Begin/End - the range of explosion indexes for this element
  unsigned Begin;
  unsigned End;

protected:
  explicit RecordField(const TypeInfo &elementTI)
    : Layout(ElementLayout::getIncomplete(elementTI)) {}

  explicit RecordField(const ElementLayout &layout,
                       unsigned begin, unsigned end)
    : Layout(layout), Begin(begin), End(end) {}

  const FieldImpl *asImpl() const {
    return static_cast<const FieldImpl*>(this);
  }
public:
  const TypeInfo &getTypeInfo() const { return Layout.getType(); }

  void completeFrom(const ElementLayout &layout) {
    Layout.completeFrom(layout);
  }

  bool isEmpty() const {
    return Layout.isEmpty();
  }

  IsTriviallyDestroyable_t isTriviallyDestroyable() const {
    return Layout.isTriviallyDestroyable();
  }

  IsABIAccessible_t isABIAccessible() const {
    return Layout.getType().isABIAccessible();
  }

  Address projectAddress(IRGenFunction &IGF, Address seq,
                         NonFixedOffsets offsets) const {
    return Layout.project(IGF, seq, offsets, "." + asImpl()->getFieldName());
  }
  
  ElementLayout::Kind getKind() const {
    return Layout.getKind();
  }

  bool hasFixedByteOffset() const {
    return Layout.hasByteOffset();
  }
  
  Size getFixedByteOffset() const {
    return Layout.getByteOffset();
  }

  unsigned getStructIndex() const { return Layout.getStructIndex(); }

  unsigned getNonFixedElementIndex() const {
    return Layout.getNonFixedElementIndex();
  }

  std::pair<unsigned, unsigned> getProjectionRange() const {
    return {Begin, End};
  }
};

enum FieldsAreABIAccessible_t : bool {
  FieldsAreNotABIAccessible = false,
  FieldsAreABIAccessible = true,
};

/// A metaprogrammed TypeInfo implementation for record types.
template <class Impl, class Base, class FieldImpl_,
          bool IsLoadable = std::is_base_of<LoadableTypeInfo, Base>::value>
class RecordTypeInfoImpl : public Base,
    private llvm::TrailingObjects<Impl, FieldImpl_> {
  friend class llvm::TrailingObjects<Impl, FieldImpl_>;

public:
  using FieldImpl = FieldImpl_;

private:
  const unsigned NumFields;
  const unsigned AreFieldsABIAccessible : 1;

  mutable Optional<const FieldImpl *> ExtraInhabitantProvidingField;
  mutable Optional<bool> MayHaveExtraInhabitants;

protected:
  const Impl &asImpl() const { return *static_cast<const Impl*>(this); }

  template <class... As> 
  RecordTypeInfoImpl(ArrayRef<FieldImpl> fields,
                     FieldsAreABIAccessible_t fieldsABIAccessible,
                     As&&...args)
      : Base(std::forward<As>(args)...),
        NumFields(fields.size()),
        AreFieldsABIAccessible(fieldsABIAccessible) {
    std::uninitialized_copy(fields.begin(), fields.end(),
                            this->template getTrailingObjects<FieldImpl>());
  }

public:
  /// Allocate and initialize a type info of this type.
  template <class... As>
  static Impl *create(ArrayRef<FieldImpl> fields, As &&...args) {
    size_t size = Impl::template totalSizeToAlloc<FieldImpl>(fields.size());
    void *buffer = ::operator new(size);
    return new(buffer) Impl(fields, std::forward<As>(args)...);
  }

  bool areFieldsABIAccessible() const {
    return AreFieldsABIAccessible;
  }

  ArrayRef<FieldImpl> getFields() const {
    return {this->template getTrailingObjects<FieldImpl>(), NumFields};
  }

  /// The standard schema is just all the fields jumbled together.
  void getSchema(ExplosionSchema &schema) const override {
    for (auto &field : getFields()) {
      field.getTypeInfo().getSchema(schema);
    }
  }

  void assignWithCopy(IRGenFunction &IGF, Address dest, Address src, SILType T,
                      bool isOutlined) const override {
    // If the fields are not ABI-accessible, use the value witness table.
    if (!AreFieldsABIAccessible) {
      return emitAssignWithCopyCall(IGF, T, dest, src);
    }

    if (isOutlined || T.hasParameterizedExistential()) {
      auto offsets = asImpl().getNonFixedOffsets(IGF, T);
      for (auto &field : getFields()) {
        if (field.isEmpty())
          continue;

        Address destField = field.projectAddress(IGF, dest, offsets);
        Address srcField = field.projectAddress(IGF, src, offsets);
        field.getTypeInfo().assignWithCopy(
            IGF, destField, srcField, field.getType(IGF.IGM, T), isOutlined);
      }
    } else {
      this->callOutlinedCopy(IGF, dest, src, T, IsNotInitialization, IsNotTake);
    }
  }

  void assignWithTake(IRGenFunction &IGF, Address dest, Address src, SILType T,
                      bool isOutlined) const override {
    // If the fields are not ABI-accessible, use the value witness table.
    if (!AreFieldsABIAccessible) {
      return emitAssignWithTakeCall(IGF, T, dest, src);
    }

    if (isOutlined || T.hasParameterizedExistential()) {
      auto offsets = asImpl().getNonFixedOffsets(IGF, T);
      for (auto &field : getFields()) {
        if (field.isEmpty())
          continue;

        Address destField = field.projectAddress(IGF, dest, offsets);
        Address srcField = field.projectAddress(IGF, src, offsets);
        field.getTypeInfo().assignWithTake(
            IGF, destField, srcField, field.getType(IGF.IGM, T), isOutlined);
      }
    } else {
      this->callOutlinedCopy(IGF, dest, src, T, IsNotInitialization, IsTake);
    }
  }

  void initializeWithCopy(IRGenFunction &IGF, Address dest, Address src,
                          SILType T, bool isOutlined) const override {
    // If we're POD, use the generic routine.
    if (this->isTriviallyDestroyable(ResilienceExpansion::Maximal) &&
        isa<LoadableTypeInfo>(this)) {
      return cast<LoadableTypeInfo>(this)->LoadableTypeInfo::initializeWithCopy(
          IGF, dest, src, T, isOutlined);
    }

    // If the fields are not ABI-accessible, use the value witness table.
    if (!AreFieldsABIAccessible) {
      return emitInitializeWithCopyCall(IGF, T, dest, src);
    }

    if (isOutlined || T.hasParameterizedExistential()) {
      auto offsets = asImpl().getNonFixedOffsets(IGF, T);
      for (auto &field : getFields()) {
        if (field.isEmpty())
          continue;

        Address destField = field.projectAddress(IGF, dest, offsets);
        Address srcField = field.projectAddress(IGF, src, offsets);
        field.getTypeInfo().initializeWithCopy(
            IGF, destField, srcField, field.getType(IGF.IGM, T), isOutlined);
      }
    } else {
      this->callOutlinedCopy(IGF, dest, src, T, IsInitialization, IsNotTake);
    }
  }

  void initializeWithTake(IRGenFunction &IGF, Address dest, Address src,
                          SILType T, bool isOutlined) const override {
    // If we're bitwise-takable, use memcpy.
    if (this->isBitwiseTakable(ResilienceExpansion::Maximal)) {
      IGF.Builder.CreateMemCpy(
          dest.getAddress(), llvm::MaybeAlign(dest.getAlignment().getValue()),
          src.getAddress(), llvm::MaybeAlign(src.getAlignment().getValue()),
          asImpl().Impl::getSize(IGF, T));
      return;
    }

    // If the fields are not ABI-accessible, use the value witness table.
    if (!AreFieldsABIAccessible) {
      return emitInitializeWithTakeCall(IGF, T, dest, src);
    }

    if (isOutlined || T.hasParameterizedExistential()) {
      auto offsets = asImpl().getNonFixedOffsets(IGF, T);
      for (auto &field : getFields()) {
        if (field.isEmpty())
          continue;

        Address destField = field.projectAddress(IGF, dest, offsets);
        Address srcField = field.projectAddress(IGF, src, offsets);
        field.getTypeInfo().initializeWithTake(
            IGF, destField, srcField, field.getType(IGF.IGM, T), isOutlined);
      }
    } else {
      this->callOutlinedCopy(IGF, dest, src, T, IsInitialization, IsTake);
    }
  }

  void destroy(IRGenFunction &IGF, Address addr, SILType T,
               bool isOutlined) const override {
    // If the fields are not ABI-accessible, use the value witness table.
    if (!AreFieldsABIAccessible) {
      return emitDestroyCall(IGF, T, addr);
    }

    if (isOutlined || T.hasParameterizedExistential()) {
      auto offsets = asImpl().getNonFixedOffsets(IGF, T);
      for (auto &field : getFields()) {
        if (field.isTriviallyDestroyable())
          continue;

        field.getTypeInfo().destroy(IGF,
                                    field.projectAddress(IGF, addr, offsets),
                                    field.getType(IGF.IGM, T), isOutlined);
      }
    } else {
      this->callOutlinedDestroy(IGF, addr, T);
    }
  }

  // The extra inhabitants of a record are determined from its fields.
  bool mayHaveExtraInhabitants(IRGenModule &IGM) const override {
    if (!MayHaveExtraInhabitants.has_value()) {
      MayHaveExtraInhabitants = false;
      for (auto &field : asImpl().getFields())
        if (field.getTypeInfo().mayHaveExtraInhabitants(IGM)) {
          MayHaveExtraInhabitants = true;
          break;
        }
    }
    return *MayHaveExtraInhabitants;
  }
  
  // Perform an operation using the field that provides extra inhabitants for
  // the aggregate, whether that field is known statically or dynamically.
  llvm::Value *withExtraInhabitantProvidingField(IRGenFunction &IGF,
         Address structAddr,
         SILType structType,
         llvm::Value *knownStructNumXI,
         llvm::Type *resultTy,
         llvm::function_ref<llvm::Value* (const FieldImpl &field,
                                          llvm::Value *numXI)> body) const {
    // If we know one field consistently provides extra inhabitants, delegate
    // to that field.
    if (auto field = asImpl().getFixedExtraInhabitantProvidingField(IGF.IGM)){
      return body(*field, knownStructNumXI);
    }
    
    // Otherwise, we have to figure out which field at runtime.

    // The number of extra inhabitants the instantiated type has can be used
    // to figure out which field the runtime chose. The runtime uses the same
    // algorithm as above--use the field with the most extra inhabitants,
    // favoring the earliest field in a tie. If we test the number of extra
    // inhabitants in the struct against each field type's, then the first
    // match should indicate which field we chose.
    //
    // We can reduce the decision space somewhat if there are fixed-layout
    // fields, since we know the only possible runtime choices are
    // either the fixed field with the most extra inhabitants (if any), or
    // one of the unknown-layout fields.
    //
    // See whether we have a fixed candidate.
    const FieldImpl *fixedCandidate = nullptr;
    unsigned fixedCount = 0;
    for (auto &field : asImpl().getFields()) {
      if (!field.getTypeInfo().mayHaveExtraInhabitants(IGF.IGM))
        continue;
      
      if (const FixedTypeInfo *fixed =
            dyn_cast<FixedTypeInfo>(&field.getTypeInfo())) {
        auto fieldCount = fixed->getFixedExtraInhabitantCount(IGF.IGM);
        if (fieldCount > fixedCount) {
          fixedCandidate = &field;
          fixedCount = fieldCount;
        }
      }
    }
    
    // Loop through checking to see whether we picked the fixed candidate
    // (if any) or one of the unknown-layout fields.
    llvm::Value *instantiatedCount
      = (knownStructNumXI
           ? knownStructNumXI
           : emitLoadOfExtraInhabitantCount(IGF, structType));
    
    auto contBB = IGF.createBasicBlock("chose_field_for_xi");
    llvm::PHINode *contPhi = nullptr;
    if (resultTy != IGF.IGM.VoidTy)
      contPhi = llvm::PHINode::Create(resultTy,
                                      asImpl().getFields().size());
    
    // If two fields have the same type, they have the same extra inhabitant
    // count, and we'll pick the first. We don't have to check both.
    SmallPtrSet<SILType, 4> visitedTypes;
    
    for (auto &field : asImpl().getFields()) {
      if (!field.getTypeInfo().mayHaveExtraInhabitants(IGF.IGM))
        continue;

      ConditionalDominanceScope condition(IGF);

      llvm::Value *fieldCount;
      if (isa<FixedTypeInfo>(field.getTypeInfo())) {
        // Skip fixed fields except for the candidate with the most known
        // extra inhabitants we picked above.
        if (&field != fixedCandidate)
          continue;
        
        fieldCount = IGF.IGM.getInt32(fixedCount);
      } else {
        auto fieldTy = field.getType(IGF.IGM, structType);
        // If this field has the same type as a field we already tested,
        // we'll never pick this one, since they both have the same count.
        if (!visitedTypes.insert(fieldTy).second)
          continue;
      
        fieldCount = emitLoadOfExtraInhabitantCount(IGF, fieldTy);
      }
      auto equalsCount = IGF.Builder.CreateICmpEQ(instantiatedCount,
                                                  fieldCount);
      
      auto yesBB = IGF.createBasicBlock("");
      auto noBB = IGF.createBasicBlock("");
      
      IGF.Builder.CreateCondBr(equalsCount, yesBB, noBB);
      
      IGF.Builder.emitBlock(yesBB);
      auto value = body(field, instantiatedCount);
      if (contPhi)
        contPhi->addIncoming(value, IGF.Builder.GetInsertBlock());
      IGF.Builder.CreateBr(contBB);
      
      IGF.Builder.emitBlock(noBB);
    }
    
    // We shouldn't have picked a number of extra inhabitants inconsistent
    // with any individual field.
    IGF.Builder.CreateUnreachable();
    
    IGF.Builder.emitBlock(contBB);
    if (contPhi)
      IGF.Builder.Insert(contPhi);
   
    return contPhi;
  }

  const FieldImpl *
  getFixedExtraInhabitantProvidingField(IRGenModule &IGM) const {
    if (!ExtraInhabitantProvidingField.has_value()) {
      unsigned mostExtraInhabitants = 0;
      const FieldImpl *fieldWithMost = nullptr;
      const FieldImpl *singleNonFixedField = nullptr;

      // TODO: If two fields have the same type, they have the same extra
      // inhabitant count, and we'll pick the first. We don't have to check
      // both. However, we don't always have access to the substituted struct
      // type from this context, which would be necessary to make that
      // judgment reliably.
      
      for (auto &field : asImpl().getFields()) {
        auto &ti = field.getTypeInfo();
        if (!ti.mayHaveExtraInhabitants(IGM))
          continue;
        
        auto *fixed = dyn_cast<FixedTypeInfo>(&field.getTypeInfo());
        // If any field is non-fixed, we can't definitively pick a best one,
        // unless it happens to be the only non-fixed field and none of the
        // other fields have extra inhabitants.
        if (!fixed) {
          // If we already saw a non-fixed field, then we can't pick one
          // at compile time.
          if (singleNonFixedField) {
            singleNonFixedField = fieldWithMost = nullptr;
            break;
          }
          
          // Otherwise, note this field for later. If we have no fixed
          // candidates, it may be the only choice for extra inhabitants.
          singleNonFixedField = &field;
          continue;
        }
        
        unsigned count = fixed->getFixedExtraInhabitantCount(IGM);
        if (count > mostExtraInhabitants) {
          mostExtraInhabitants = count;
          fieldWithMost = &field;
        }
      }
      
      if (fieldWithMost) {
        if (singleNonFixedField) {
          // If we have a non-fixed and fixed candidate, we can't know for
          // sure now.
          ExtraInhabitantProvidingField = nullptr;
        } else {
          // If we had all fixed fields, pick the one with the most extra
          // inhabitants.
          ExtraInhabitantProvidingField = fieldWithMost;
        }
      } else {
        // If there were no fixed candidates, but we had a single non-fixed
        // field with potential extra inhabitants, then it's our only choice.
        ExtraInhabitantProvidingField = singleNonFixedField;
      }
    }
    return *ExtraInhabitantProvidingField;
  }

  void collectMetadataForOutlining(OutliningMetadataCollector &collector,
                                   SILType T) const override {
    for (auto &field : getFields()) {
      if (field.isEmpty())
        continue;
      auto fType = field.getType(collector.IGF.IGM, T);
      field.getTypeInfo().collectMetadataForOutlining(collector, fType);
    }
    collector.collectTypeMetadataForLayout(T);
  }
};

template <class Impl, class Base, class FieldImpl_,
          bool IsFixedSize = std::is_base_of<FixedTypeInfo, Base>::value,
          bool IsLoadable = std::is_base_of<LoadableTypeInfo, Base>::value>
class RecordTypeInfo;

/// An implementation of RecordTypeInfo for non-fixed-size types
/// (but not resilient ones where we don't know the complete set of
/// stored properties).
///
/// Override the buffer operations to just delegate to the unique
/// non-empty field, if there is one.
template <class Impl, class Base, class FieldImpl>
class RecordTypeInfo<Impl, Base, FieldImpl,
                     /*IsFixedSize*/ false, /*IsLoadable*/ false>
    : public RecordTypeInfoImpl<Impl, Base, FieldImpl> {
  using super = RecordTypeInfoImpl<Impl, Base, FieldImpl>;

  /// The index+1 of the unique non-empty field, or zero if there is none.
  unsigned UniqueNonEmptyFieldIndexPlusOne;
protected:
  template <class... As>
  RecordTypeInfo(ArrayRef<FieldImpl> fields, As&&...args)
      : super(fields, std::forward<As>(args)...) {

    // Look for a unique non-empty field.
    UniqueNonEmptyFieldIndexPlusOne = findUniqueNonEmptyField(fields);
  }

public:
  using super::getStorageType;

  Address initializeBufferWithCopyOfBuffer(IRGenFunction &IGF,
                                           Address destBuffer,
                                           Address srcBuffer,
                                           SILType type) const override {
    if (auto field = getUniqueNonEmptyField()) {
      auto &fieldTI = field->getTypeInfo();
      Address fieldResult =
        fieldTI.initializeBufferWithCopyOfBuffer(IGF, destBuffer, srcBuffer,
                                                 field->getType(IGF.IGM, type));
      return IGF.Builder.CreateElementBitCast(fieldResult, getStorageType());
    } else {
      return super::initializeBufferWithCopyOfBuffer(IGF, destBuffer,
                                                     srcBuffer, type);
    }
  }

private:
  static unsigned findUniqueNonEmptyField(ArrayRef<FieldImpl> fields) {
    unsigned result = 0;
    for (auto &field : fields) {
      // Ignore empty fields.
      if (field.isEmpty()) continue;

      // If the field is not ABI-accessible, suppress this.
      if (!field.isABIAccessible()) return 0;

      // If we've already found an index, then there isn't a
      // unique non-empty field.
      if (result) return 0;

      result = (&field - fields.data()) + 1;
    }

    return result;
  }

  const FieldImpl *getUniqueNonEmptyField() const {
    if (UniqueNonEmptyFieldIndexPlusOne) {
      return &this->getFields()[UniqueNonEmptyFieldIndexPlusOne - 1];
    } else {
      return nullptr;
    }
  }
};

/// An implementation of RecordTypeInfo for fixed-layout types that
/// aren't necessarily loadable.
template <class Impl, class Base, class FieldImpl>
class RecordTypeInfo<Impl, Base, FieldImpl,
                     /*IsFixedSize*/ true, /*IsLoadable*/ false>
    : public RecordTypeInfoImpl<Impl, Base, FieldImpl> {
  using super = RecordTypeInfoImpl<Impl, Base, FieldImpl>;
protected:
  template <class... As> 
  RecordTypeInfo(ArrayRef<FieldImpl> fields, As &&...args)
    : super(fields, FieldsAreABIAccessible, std::forward<As>(args)...) {}

  using super::asImpl;

public:
  unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const override {
    if (auto field = asImpl().getFixedExtraInhabitantProvidingField(IGM)) {
      auto &fieldTI = cast<FixedTypeInfo>(field->getTypeInfo());
      return fieldTI.getFixedExtraInhabitantCount(IGM);
    }
    
    return 0;
  }

  bool canValueWitnessExtraInhabitantsUpTo(IRGenModule &IGM,
                                           unsigned index) const override {
    if (auto field = asImpl().getFixedExtraInhabitantProvidingField(IGM)) {
      // The non-extra-inhabitant-providing fields of the type must be
      // trivial, because an enum may contain garbage values in those fields'
      // storage which the value witness operation won't handle.
      for (auto &otherField : asImpl().getFields()) {
        if (field == &otherField)
          continue;
        auto &ti = otherField.getTypeInfo();
        if (!ti.isTriviallyDestroyable(ResilienceExpansion::Maximal)) {
          return false;
        }
      }

      return field->getTypeInfo()
        .canValueWitnessExtraInhabitantsUpTo(IGM, index);
    }
    
    return false;
  }

  APInt getFixedExtraInhabitantValue(IRGenModule &IGM,
                                     unsigned bits,
                                     unsigned index) const override {
    // We are only called if the type is known statically to have extra
    // inhabitants.
    auto &field = *asImpl().getFixedExtraInhabitantProvidingField(IGM);
    auto &fieldTI = cast<FixedTypeInfo>(field.getTypeInfo());
    auto fieldSize = fieldTI.getFixedExtraInhabitantMask(IGM).getBitWidth();

    auto value = BitPatternBuilder(IGM.Triple.isLittleEndian());
    value.appendClearBits(field.getFixedByteOffset().getValueInBits());
    value.append(fieldTI.getFixedExtraInhabitantValue(IGM, fieldSize, index));
    value.padWithClearBitsTo(bits);
    return value.build().value();
  }

  APInt getFixedExtraInhabitantMask(IRGenModule &IGM) const override {
    auto field = asImpl().getFixedExtraInhabitantProvidingField(IGM);
    if (!field)
      return APInt();

    const FixedTypeInfo &fieldTI
      = cast<FixedTypeInfo>(field->getTypeInfo());
    auto targetSize = asImpl().getFixedSize().getValueInBits();

    if (fieldTI.isKnownEmpty(ResilienceExpansion::Maximal))
      return APInt(targetSize, 0);

    auto mask = BitPatternBuilder(IGM.Triple.isLittleEndian());
    mask.appendClearBits(field->getFixedByteOffset().getValueInBits());
    mask.append(fieldTI.getFixedExtraInhabitantMask(IGM));
    mask.padWithClearBitsTo(targetSize);
    return mask.build().value();
  }

  llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF,
                                       Address structAddr,
                                       SILType structType,
                                       bool isOutlined) const override {
    auto field = *asImpl().getFixedExtraInhabitantProvidingField(IGF.IGM);
    Address fieldAddr =
      asImpl().projectFieldAddress(IGF, structAddr, structType, field);
    auto &fieldTI = cast<FixedTypeInfo>(field.getTypeInfo());
    return fieldTI.getExtraInhabitantIndex(IGF, fieldAddr,
                                     field.getType(IGF.IGM, structType),
                                     false /*not outlined for field*/);
  }

  void storeExtraInhabitant(IRGenFunction &IGF,
                            llvm::Value *index,
                            Address structAddr,
                            SILType structType,
                            bool isOutlined) const override {
    auto field = *asImpl().getFixedExtraInhabitantProvidingField(IGF.IGM);
    Address fieldAddr =
      asImpl().projectFieldAddress(IGF, structAddr, structType, field);
    auto &fieldTI = cast<FixedTypeInfo>(field.getTypeInfo());
    fieldTI.storeExtraInhabitant(IGF, index, fieldAddr,
                                 field.getType(IGF.IGM, structType),
                                 false /*not outlined for field*/);
  }
};

/// An implementation of RecordTypeInfo for loadable types. 
template <class Impl, class Base, class FieldImpl>
class RecordTypeInfo<Impl, Base, FieldImpl,
                     /*IsFixedSize*/ true, /*IsLoadable*/ true>
    : public RecordTypeInfo<Impl, Base, FieldImpl, true, false> {
  using super = RecordTypeInfo<Impl, Base, FieldImpl, true, false>;

  unsigned ExplosionSize : 16;

protected:
  using super::asImpl;

  template <class... As> 
  RecordTypeInfo(ArrayRef<FieldImpl> fields,
                 unsigned explosionSize,
                 As &&...args)
    : super(fields, std::forward<As>(args)...),
      ExplosionSize(explosionSize) {}

private:
  template <void (LoadableTypeInfo::*Op)(IRGenFunction &IGF,
                                         Address addr,
                                         Explosion &out) const>
  void forAllFields(IRGenFunction &IGF, Address addr, Explosion &out) const {
    auto offsets = asImpl().getNonFixedOffsets(IGF);
    for (auto &field : getFields()) {
      if (field.isEmpty()) continue;

      Address fieldAddr = field.projectAddress(IGF, addr, offsets);
      (cast<LoadableTypeInfo>(field.getTypeInfo()).*Op)(IGF, fieldAddr, out);
    }
  }

  template <void (LoadableTypeInfo::*Op)(IRGenFunction &IGF, Address addr,
                                         Explosion &out, Atomicity atomicity) const>
  void forAllFields(IRGenFunction &IGF, Address addr, Explosion &out,
                    Atomicity atomicity) const {
    auto offsets = asImpl().getNonFixedOffsets(IGF);
    for (auto &field : getFields()) {
      if (field.isEmpty()) continue;

      Address fieldAddr = field.projectAddress(IGF, addr, offsets);
      (cast<LoadableTypeInfo>(field.getTypeInfo()).*Op)(IGF, fieldAddr, out,
                                                        atomicity);
    }
  }

  template <void (LoadableTypeInfo::*Op)(IRGenFunction &IGF, Explosion &in,
                                         Address addr, bool isOutlined) const>
  void forAllFields(IRGenFunction &IGF, Explosion &in, Address addr,
                    bool isOutlined) const {
    auto offsets = asImpl().getNonFixedOffsets(IGF);
    for (auto &field : getFields()) {
      if (field.isEmpty()) continue;

      Address fieldAddr = field.projectAddress(IGF, addr, offsets);
      (cast<LoadableTypeInfo>(field.getTypeInfo()).*Op)(IGF, in, fieldAddr,
                                                        isOutlined);
    }
  }

  template <void (LoadableTypeInfo::*Op)(IRGenFunction &IGF, Explosion &in,
                                         Address addr, bool isOutlined,
                                         SILType T) const>
  void forAllFields(IRGenFunction &IGF, Explosion &in, Address addr,
                    bool isOutlined, SILType T) const {
    auto offsets = asImpl().getNonFixedOffsets(IGF);
    for (auto &field : getFields()) {
      if (field.isEmpty()) continue;

      Address fieldAddr = field.projectAddress(IGF, addr, offsets);
      (cast<LoadableTypeInfo>(field.getTypeInfo()).*Op)(IGF, in, fieldAddr,
                                                    isOutlined,
                                                    field.getType(IGF.IGM, T));
    }
  }

public:
  using super::getFields;

  void loadAsCopy(IRGenFunction &IGF, Address addr,
                  Explosion &out) const override {
    forAllFields<&LoadableTypeInfo::loadAsCopy>(IGF, addr, out);
  }

  void loadAsTake(IRGenFunction &IGF, Address addr,
                  Explosion &out) const override {
    forAllFields<&LoadableTypeInfo::loadAsTake>(IGF, addr, out);
  }

  void assign(IRGenFunction &IGF, Explosion &e, Address addr,
              bool isOutlined, SILType T) const override {
    forAllFields<&LoadableTypeInfo::assign>(IGF, e, addr, isOutlined, T);
  }

  void initialize(IRGenFunction &IGF, Explosion &e, Address addr,
                  bool isOutlined) const override {
    forAllFields<&LoadableTypeInfo::initialize>(IGF, e, addr, isOutlined);
  }

  unsigned getExplosionSize() const override {
    return ExplosionSize;
  }

  void reexplode(Explosion &src,
                 Explosion &dest) const override {
    for (auto &field : getFields())
      cast<LoadableTypeInfo>(field.getTypeInfo()).reexplode(src, dest);
  }

  void copy(IRGenFunction &IGF, Explosion &src,
            Explosion &dest, Atomicity atomicity) const override {
    for (auto &field : getFields())
      cast<LoadableTypeInfo>(field.getTypeInfo())
          .copy(IGF, src, dest, atomicity);
  }

  void consume(IRGenFunction &IGF, Explosion &src,
               Atomicity atomicity, SILType T) const override {
    for (auto &field : getFields()) {
      cast<LoadableTypeInfo>(field.getTypeInfo())
          .consume(IGF, src, atomicity, field.getType(IGF.IGM, T));
    }
  }

  void fixLifetime(IRGenFunction &IGF, Explosion &src) const override {
    for (auto &field : getFields())
      cast<LoadableTypeInfo>(field.getTypeInfo()).fixLifetime(IGF, src);
  }
  
  void packIntoEnumPayload(IRGenModule &IGM,
                           IRBuilder &builder,
                           EnumPayload &payload,
                           Explosion &src,
                           unsigned startOffset) const override {
    for (auto &field : getFields()) {
      if (!field.isEmpty()) {
        unsigned offset = field.getFixedByteOffset().getValueInBits()
          + startOffset;
        cast<LoadableTypeInfo>(field.getTypeInfo())
          .packIntoEnumPayload(IGM, builder, payload, src, offset);
      }
    }
  }
  
  void unpackFromEnumPayload(IRGenFunction &IGF, const EnumPayload &payload,
                             Explosion &dest, unsigned startOffset)
                            const override {
    for (auto &field : getFields()) {
      if (!field.isEmpty()) {
        unsigned offset = field.getFixedByteOffset().getValueInBits()
          + startOffset;
        cast<LoadableTypeInfo>(field.getTypeInfo())
          .unpackFromEnumPayload(IGF, payload, dest, offset);
      }
    }
  }
};

/// A builder of record types.
///
/// Required for a full implementation:
///   TypeInfoImpl *construct(void *buffer, ArrayRef<ASTField> fields);
///   FieldImpl getFieldInfo(const ASTField &field, const TypeInfo &fieldTI);
///   Type getType(const ASTField &field);
///   void performLayout(ArrayRef<const TypeInfo *> fieldTypes);
///     - should call recordLayout with the layout
template <class BuilderImpl, class FieldImpl, class ASTField>
class RecordTypeBuilder {
protected:
  IRGenModule &IGM;
  RecordTypeBuilder(IRGenModule &IGM) : IGM(IGM) {}

  BuilderImpl *asImpl() { return static_cast<BuilderImpl*>(this); }

public:
  TypeInfo *layout(ArrayRef<ASTField> astFields) {
    SmallVector<FieldImpl, 8> fields;
    SmallVector<const TypeInfo *, 8> fieldTypesForLayout;
    fields.reserve(astFields.size());
    fieldTypesForLayout.reserve(astFields.size());

    bool loadable = true;
    auto fieldsABIAccessible = FieldsAreABIAccessible;

    unsigned explosionSize = 0;
    for (unsigned i : indices(astFields)) {
      auto &astField = astFields[i];
      // Compute the field's type info.
      auto &fieldTI = IGM.getTypeInfo(asImpl()->getType(astField));
      fieldTypesForLayout.push_back(&fieldTI);

      if (!fieldTI.isABIAccessible())
        fieldsABIAccessible = FieldsAreNotABIAccessible;

      fields.push_back(FieldImpl(asImpl()->getFieldInfo(i, astField, fieldTI)));

      auto loadableFieldTI = dyn_cast<LoadableTypeInfo>(&fieldTI);
      if (!loadableFieldTI) {
        loadable = false;
        continue;
      }

      auto &fieldInfo = fields.back();
      fieldInfo.Begin = explosionSize;
      bool overflow = false;
      explosionSize = llvm::SaturatingAdd(explosionSize, loadableFieldTI->getExplosionSize(), &overflow);
      if (overflow) {
        IGM.Context.Diags.diagnose(SourceLoc(), diag::explosion_size_oveflow);
      }

      fieldInfo.End = explosionSize;
    }

    // Perform layout and fill in the fields.
    StructLayout layout = asImpl()->performLayout(fieldTypesForLayout);
    for (unsigned i = 0, e = fields.size(); i != e; ++i) {
      fields[i].completeFrom(layout.getElements()[i]);
    }

    // Create the type info.
    if (loadable) {
      assert(layout.isFixedLayout());
      assert(fieldsABIAccessible);
      return asImpl()->createLoadable(fields, std::move(layout), explosionSize);
    } else if (layout.isFixedLayout()) {
      assert(fieldsABIAccessible);
      return asImpl()->createFixed(fields, std::move(layout));
    } else {
      return asImpl()->createNonFixed(fields, fieldsABIAccessible,
                                      std::move(layout));
    }
  }  
};

} // end namespace irgen
} // end namespace swift

#endif

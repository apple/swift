//===--- ScalarPairTypeInfo.h - Type info for scalar pairs ------*- C++ -*-===//
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
// This file defines ScalarPairTypeInfo, which is a convenient abstract
// implementation of TypeInfo for working with types that are
// efficiently scalarizable.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_SCALARPAIRTYPEINFO_H
#define SWIFT_IRGEN_SCALARPAIRTYPEINFO_H

#include "NativeConventionSchema.h"
#include "ScalarTypeInfo.h"

namespace swift {
namespace irgen {

template <class Derived, class Base>
class ScalarPairTypeInfo : public ScalarTypeInfo<Derived, Base> {
  using super = ScalarTypeInfo<Derived, Base>;
protected:
  template <class... T> ScalarPairTypeInfo(T &&...args)
    : super(::std::forward<T>(args)...) {}

  const Derived &asDerived() const {
    return static_cast<const Derived &>(*this);
  }

public:
  llvm::StructType *getStorageType() const {
    return cast<llvm::StructType>(TypeInfo::getStorageType());
  }

  Address projectFirstElement(IRGenFunction &IGF, Address address) const {
    return IGF.Builder.CreateStructGEP(address, 0, Size(0),
                                       address->getName()
                                         + asDerived().getFirstElementLabel());
  }

  Address projectSecondElement(IRGenFunction &IGF, Address address) const {
    return IGF.Builder.CreateStructGEP(address, 1,
                 asDerived().getSecondElementOffset(IGF.IGM),
                 address->getName() + asDerived().getSecondElementLabel());
  }

  unsigned getExplosionSize() const override {
    return 2;
  }

  void getSchema(ExplosionSchema &schema) const override {
    llvm::StructType *structTy = getStorageType();
    schema.add(ExplosionSchema::Element::forScalar(structTy->getElementType(0)));
    schema.add(ExplosionSchema::Element::forScalar(structTy->getElementType(1)));
  }

  void addToAggLowering(IRGenModule &IGM, SwiftAggLowering &lowering,
                        Size offset) const override {
    llvm::StructType *structTy = getStorageType();
    this->addScalarToAggLowering(IGM, lowering, structTy->getElementType(0),
                                 offset, asDerived().getFirstElementSize(IGM));
    this->addScalarToAggLowering(IGM, lowering, structTy->getElementType(1),
                              offset + asDerived().getSecondElementOffset(IGM),
                                 asDerived().getSecondElementSize(IGM));
  }

  void loadAsCopy(IRGenFunction &IGF, Address address,
                  Explosion &e) const override {
    Address firstAddr = projectFirstElement(IGF, address);
    auto first =
      IGF.Builder.CreateLoad(firstAddr, firstAddr->getName() + ".load");
    asDerived().emitRetainFirstElement(IGF, first);
    e.add(first);

    Address secondAddr = projectSecondElement(IGF, address);
    auto second = IGF.Builder.CreateLoad(secondAddr);
    asDerived().emitRetainSecondElement(IGF, second);
    e.add(second);
  }

  void loadAsTake(IRGenFunction &IGF, Address addr,
                  Explosion &e) const override {
    // Load the function.
    Address firstAddr = projectFirstElement(IGF, addr);
    e.add(IGF.Builder.CreateLoad(firstAddr));

    Address secondAddr = projectSecondElement(IGF, addr);
    e.add(IGF.Builder.CreateLoad(secondAddr));
  }

  void assign(IRGenFunction &IGF, Explosion &e, Address address,
              bool isOutlined, SILType T) const override {
    // Store the function pointer.
    Address firstAddr = projectFirstElement(IGF, address);
    asDerived().emitAssignFirstElement(IGF, e.claimNext(), firstAddr);

    Address secondAddr = projectSecondElement(IGF, address);
    asDerived().emitAssignSecondElement(IGF, e.claimNext(), secondAddr);
  }

  void initialize(IRGenFunction &IGF, Explosion &e, Address address,
                  bool isOutlined) const override {
    Address firstAddr = projectFirstElement(IGF, address);
    IGF.Builder.CreateStore(e.claimNext(), firstAddr);

    Address secondAddr = projectSecondElement(IGF, address);
    IGF.Builder.CreateStore(e.claimNext(), secondAddr);
  }

  void copy(IRGenFunction &IGF, Explosion &src,
            Explosion &dest, Atomicity atomicity) const override {
    auto first = src.claimNext();
    asDerived().emitRetainFirstElement(IGF, first, atomicity);
    dest.add(first);

    auto second = src.claimNext();
    asDerived().emitRetainSecondElement(IGF, second, atomicity);
    dest.add(second);
  }

  void consume(IRGenFunction &IGF, Explosion &src,
               Atomicity atomicity, SILType T) const override {
    auto first = src.claimNext();
    asDerived().emitReleaseFirstElement(IGF, first, atomicity);

    auto second = src.claimNext();
    asDerived().emitReleaseSecondElement(IGF, second, atomicity);
  }

  void fixLifetime(IRGenFunction &IGF, Explosion &src) const override {
    auto first = src.claimNext();
    if (!asDerived().isFirstElementTrivial())
      IGF.emitFixLifetime(first);

    auto second = src.claimNext();
    if (!asDerived().isSecondElementTrivial())
      IGF.emitFixLifetime(second);
  }

  void destroy(IRGenFunction &IGF, Address addr, SILType T,
               bool isOutlined) const override {
    if (!asDerived().isFirstElementTrivial()) {
      auto first = IGF.Builder.CreateLoad(projectFirstElement(IGF, addr));
      asDerived().emitReleaseFirstElement(IGF, first);
    }

    if (!asDerived().isSecondElementTrivial()) {
      auto first = IGF.Builder.CreateLoad(projectSecondElement(IGF, addr));
      asDerived().emitReleaseSecondElement(IGF, first);
    }
  }

  void packIntoEnumPayload(IRGenModule &IGM,
                           IRBuilder &builder,
                           EnumPayload &payload,
                           Explosion &src,
                           unsigned offset) const override {
    payload.insertValue(IGM, builder, src.claimNext(), offset);
    payload.insertValue(IGM, builder, src.claimNext(),
      offset + asDerived().getSecondElementOffset(IGM).getValueInBits());
  }
  
  void unpackFromEnumPayload(IRGenFunction &IGF,
                             const EnumPayload &payload,
                             Explosion &dest,
                             unsigned offset) const override {
    auto storageTy = getStorageType();
    dest.add(payload.extractValue(IGF, storageTy->getElementType(0), offset));
    dest.add(payload.extractValue(IGF, storageTy->getElementType(1),
      offset + asDerived().getSecondElementOffset(IGF.IGM).getValueInBits()));
  }
};

template <class Derived, class Base>
class TrivialScalarPairTypeInfo : public ScalarPairTypeInfo<Derived, Base> {
  using super = ScalarPairTypeInfo<Derived, Base>;
protected:
  template <class... T> TrivialScalarPairTypeInfo(T &&...args)
    : super(::std::forward<T>(args)...) {}

  const Derived &asDerived() const {
    return static_cast<const Derived &>(*this);
  }

public:
  static bool isFirstElementTrivial() {
    return true;
  }
  void emitRetainFirstElement(
      IRGenFunction &IGF, llvm::Value *value,
      llvm::Optional<Atomicity> atomicity = llvm::None) const {}
  void emitReleaseFirstElement(
      IRGenFunction &IGF, llvm::Value *value,
      llvm::Optional<Atomicity> atomicity = llvm::None) const {}
  void emitAssignFirstElement(IRGenFunction &IGF, llvm::Value *value,
                              Address valueAddr) const {
    IGF.Builder.CreateStore(value, valueAddr);
  }

  static bool isSecondElementTrivial() {
    return true;
  }
  void emitRetainSecondElement(
      IRGenFunction &IGF, llvm::Value *value,
      llvm::Optional<Atomicity> atomicity = llvm::None) const {}
  void emitReleaseSecondElement(
      IRGenFunction &IGF, llvm::Value *value,
      llvm::Optional<Atomicity> atomicity = llvm::None) const {}
  void emitAssignSecondElement(IRGenFunction &IGF, llvm::Value *value,
                              Address valueAddr) const {
    IGF.Builder.CreateStore(value, valueAddr);
  }
};

}
}

#endif

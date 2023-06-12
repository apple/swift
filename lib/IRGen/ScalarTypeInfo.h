//===--- ScalarTypeInfo.h - Convenience class for scalar types --*- C++ -*-===//
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
// This file defines ScalarTypeInfo, which is a convenient abstract
// implementation of TypeInfo for working with types that are
// efficiently scalarizable.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_SCALARTYPEINFO_H
#define SWIFT_IRGEN_SCALARTYPEINFO_H

#include "EnumPayload.h"
#include "Explosion.h"
#include "TypeInfo.h"
#include "IRGenFunction.h"
#include "GenEnum.h"

namespace swift {
namespace irgen {

/// ScalarTypeInfo - An abstract class designed for use when
/// implementing a type which can be efficiently exploded and
/// unexploded.
template <class Derived, class Base>
class ScalarTypeInfo : public Base {
protected:
  template <class... T> ScalarTypeInfo(T &&...args)
    : Base(::std::forward<T>(args)...) {}

  const Derived &asDerived() const {
    return static_cast<const Derived &>(*this);
  }

public:
  void initializeFromParams(IRGenFunction &IGF, Explosion &params, Address dest,
                            SILType T, bool isOutlined) const override {
    asDerived().Derived::initialize(IGF, params, dest, isOutlined);
  }

  void initializeWithCopy(IRGenFunction &IGF, Address dest, Address src,
                          SILType T, bool isOutlined) const override {
    Explosion temp;
    asDerived().Derived::loadAsCopy(IGF, src, temp);
    asDerived().Derived::initialize(IGF, temp, dest, isOutlined);
  }

  void assignWithCopy(IRGenFunction &IGF, Address dest, Address src, SILType T,
                      bool isOutlined) const override {
    Explosion temp;
    asDerived().Derived::loadAsCopy(IGF, src, temp);
    asDerived().Derived::assign(IGF, temp, dest, isOutlined, T);
  }

  void assignWithTake(IRGenFunction &IGF, Address dest, Address src, SILType T,
                      bool isOutlined) const override {
    Explosion temp;
    asDerived().Derived::loadAsTake(IGF, src, temp);
    asDerived().Derived::assign(IGF, temp, dest, isOutlined, T);
  }

  void reexplode(Explosion &in,
                 Explosion &out) const override {
    unsigned size = asDerived().Derived::getExplosionSize();
    in.transferInto(out, size);
  }
};

/// SingleScalarTypeInfo - A further specialization of
/// ScalarTypeInfo for types which consist of a single scalar
/// which equals the storage type.
template <class Derived, class Base>
class SingleScalarTypeInfo : public ScalarTypeInfo<Derived, Base> {
protected:
  template <class... T> SingleScalarTypeInfo(T &&...args)
    : ScalarTypeInfo<Derived,Base>(::std::forward<T>(args)...) {}

  const Derived &asDerived() const {
    return static_cast<const Derived &>(*this);
  }

public:
  /// Return the type of the scalar.  Override this if it's not
  /// just the storage type.
  llvm::Type *getScalarType() const { return this->getStorageType(); }

  /// Project to the address of the scalar.  Override this if it's not
  /// just the storage type.
  Address projectScalar(IRGenFunction &IGF, Address addr) const { return addr; }

  // Subclasses must implement the following four operations:

  // Is the scalar trivially destructible?
  // static const bool IsScalarTriviallyDestroyable;

  // Make the scalar +1.
  // void emitScalarRetain(IRGenFunction &IGF, llvm::Value *value) const;

  // Make the scalar -1.
  // void emitScalarRelease(IRGenFunction &IGF, llvm::Value *value) const;

  unsigned getExplosionSize() const override {
    return 1;
  }

  void getSchema(ExplosionSchema &schema) const override {
    llvm::Type *ty = asDerived().getScalarType();
    schema.add(ExplosionSchema::Element::forScalar(ty));
  }

  void storeAsBytes(IRGenFunction &IGF, Explosion &src, Address addr) const {
    auto &IGM = IGF.IGM;

    // Store in multiples of bytes to avoid undefined bits.
    auto storageTy = addr.getElementType();
    if (storageTy->isIntegerTy() && (storageTy->getIntegerBitWidth() % 8)) {
      auto &Builder = IGF.Builder;
      auto nextByteSize = (storageTy->getIntegerBitWidth() + 7) & ~7UL;
      auto nextByteSizedIntTy =
          llvm::IntegerType::get(IGM.getLLVMContext(), nextByteSize);
      auto newAddr =
          Address(Builder.CreatePointerCast(addr.getAddress(),
                                            nextByteSizedIntTy->getPointerTo()),
                  nextByteSizedIntTy, addr.getAlignment());
      auto newValue = Builder.CreateZExt(src.claimNext(), nextByteSizedIntTy);
      Builder.CreateStore(newValue, newAddr);
      return;
    }

    IGF.Builder.CreateStore(src.claimNext(), addr);
  }

  void initialize(IRGenFunction &IGF, Explosion &src, Address addr,
                  bool isOutlined) const override {
    addr = asDerived().projectScalar(IGF, addr);

    storeAsBytes(IGF, src, addr);
  }

  void loadAsCopy(IRGenFunction &IGF, Address addr,
                  Explosion &out) const override {
    addr = asDerived().projectScalar(IGF, addr);
    llvm::Value *value = IGF.Builder.CreateLoad(addr);
    asDerived().emitScalarRetain(IGF, value, IGF.getDefaultAtomicity());
    out.add(value);
  }

  void loadAsTake(IRGenFunction &IGF, Address addr,
                  Explosion &out) const override {
    addr = asDerived().projectScalar(IGF, addr);
    out.add(IGF.Builder.CreateLoad(addr));
  }

  void assign(IRGenFunction &IGF, Explosion &src, Address dest,
              bool isOutlined, SILType T) const override {
    // Project down.
    dest = asDerived().projectScalar(IGF, dest);

    // Grab the old value if we need to.
    llvm::Value *oldValue = nullptr;
    if (!Derived::IsScalarTriviallyDestroyable) {
      oldValue = IGF.Builder.CreateLoad(dest, "oldValue");
    }

    // Store.
    storeAsBytes(IGF, src, dest);

    // Release the old value if we need to.
    if (!Derived::IsScalarTriviallyDestroyable) {
      asDerived().emitScalarRelease(IGF, oldValue, IGF.getDefaultAtomicity());
    }
  }

  void copy(IRGenFunction &IGF, Explosion &in, Explosion &out,
            Atomicity atomicity) const override {
    llvm::Value *value = in.claimNext();
    asDerived().emitScalarRetain(IGF, value, atomicity);
    out.add(value);
  }

  void consume(IRGenFunction &IGF, Explosion &in,
               Atomicity atomicity, SILType T) const override {
    llvm::Value *value = in.claimNext();
    asDerived().emitScalarRelease(IGF, value, atomicity);
  }

  void fixLifetime(IRGenFunction &IGF, Explosion &in) const override {
    llvm::Value *value = in.claimNext();
    asDerived().emitScalarFixLifetime(IGF, value);
  }

  void destroy(IRGenFunction &IGF, Address addr, SILType T,
               bool isOutlined) const override {
    if (!Derived::IsScalarTriviallyDestroyable) {
      addr = asDerived().projectScalar(IGF, addr);
      llvm::Value *value = IGF.Builder.CreateLoad(addr, "toDestroy");
      asDerived().emitScalarRelease(IGF, value, IGF.getDefaultAtomicity());
    }
  }
  
  void packIntoEnumPayload(IRGenModule &IGM,
                           IRBuilder &builder,
                           EnumPayload &payload,
                           Explosion &src,
                           unsigned offset) const override {
    payload.insertValue(IGM, builder, src.claimNext(), offset);
  }
  
  void unpackFromEnumPayload(IRGenFunction &IGF,
                             const EnumPayload &payload,
                             Explosion &dest,
                             unsigned offset) const override {
    dest.add(payload.extractValue(IGF, asDerived().getScalarType(), offset));
  }

  void addToAggLowering(IRGenModule &IGM, SwiftAggLowering &lowering,
                        Size offset) const override {
    // Can't use getFixedSize because it returns the alloc size not the store
    // size.
    LoadableTypeInfo::addScalarToAggLowering(
        IGM, lowering, asDerived().getScalarType(), offset,
        Size(IGM.DataLayout.getTypeStoreSize(asDerived().getScalarType())));
  }

};

/// SingleScalarTypeInfoWithTypeLayout - A further specialization of
/// SingleScalarTypeInfo for types which knows how-to construct a type layout
/// from its derived type which must be a TypeInfo.
template <class Derived, class Base>
class SingleScalarTypeInfoWithTypeLayout
    : public SingleScalarTypeInfo<Derived, Base> {
protected:
  template <class... T>
  SingleScalarTypeInfoWithTypeLayout(ScalarKind kind, T &&... args)
      : SingleScalarTypeInfo<Derived, Base>(::std::forward<T>(args)...),
        kind(kind) {}

  const Derived &asDerived() const {
    return static_cast<const Derived &>(*this);
  }

public:
  friend class SingleScalarTypeInfo<Derived, Base>;

  TypeLayoutEntry *
  buildTypeLayoutEntry(IRGenModule &IGM,
                       SILType T,
                       bool useStructLayouts) const override {
    if (!useStructLayouts) {
      return IGM.typeLayoutCache.getOrCreateTypeInfoBasedEntry(*this, T);
    }
    return IGM.typeLayoutCache.getOrCreateScalarEntry(asDerived(), T, kind);
  }

private:
  ScalarKind kind;
};

/// PODSingleScalarTypeInfo - A further specialization of
/// SingleScalarTypeInfo for types which consist of a single POD
/// scalar.  This is a complete implementation.
template <class Derived, class Base>
class PODSingleScalarTypeInfo : public SingleScalarTypeInfo<Derived, Base> {
protected:
  template <class StorageType, class... T> 
  PODSingleScalarTypeInfo(StorageType *storage, Size size,
                          SpareBitVector spareBits,
                          Alignment align, T &&...args)
    : SingleScalarTypeInfo<Derived, Base>(storage, size, spareBits, align,
                                          IsTriviallyDestroyable,
                                          IsCopyable,
                                          IsFixedSize,
                                          ::std::forward<T>(args)...) {}

  const Derived &asDerived() const {
    return static_cast<const Derived &>(*this);
  }

private:
  friend class SingleScalarTypeInfo<Derived, Base>;
  static const bool IsScalarTriviallyDestroyable = true;

  void emitScalarRetain(IRGenFunction &IGF, llvm::Value *value,
                        Atomicity atomicity) const {}

  void emitScalarRelease(IRGenFunction &IGF, llvm::Value *value,
                         Atomicity atomicity) const {}

  void emitScalarFixLifetime(IRGenFunction &IGF, llvm::Value *value) const {
  }

  TypeLayoutEntry *
  buildTypeLayoutEntry(IRGenModule &IGM,
                       SILType T,
                       bool useStructLayouts = false) const override {
    if (!useStructLayouts) {
      return IGM.typeLayoutCache.getOrCreateTypeInfoBasedEntry(asDerived(), T);
    }
    return IGM.typeLayoutCache.getOrCreateScalarEntry(asDerived(), T,
                                                      ScalarKind::TriviallyDestroyable);
  }

};

}
}

#endif

//===--- HeapTypeInfo.h - Utilities for reference-counted types -*- C++ -*-===//
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
// This file defines some routines that are useful for emitting
// types that are single, reference-counted pointers.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_HEAPTYPEINFO_H
#define SWIFT_IRGEN_HEAPTYPEINFO_H

#include "llvm/IR/DerivedTypes.h"
#include "ExtraInhabitants.h"
#include "ReferenceTypeInfo.h"
#include "ScalarTypeInfo.h"
#include "SwiftTargetInfo.h"
#include "GenType.h"

namespace swift {
namespace irgen {

/// \group HeapTypeInfo
  
/// The kind of 'isa' encoding a heap object uses to reference its heap
/// metadata.
enum class IsaEncoding : uint8_t {
  /// The object stores a plain pointer to its heap metadata as its first word.
  Pointer,
  /// The object's isa is managed by the Objective-C runtime and must be
  /// accessed with object_getClass. This is a superset of "Pointer" because
  /// object_getClass is compatible with pointer isas.
  ObjC,
  /// The isa encoding is unknown and must be accessed in a maximally-compatible
  /// way.
  Unknown = ObjC,
};

/// HeapTypeInfo - A type designed for use implementing a type
/// which consists solely of something reference-counted.
///
/// Subclasses should implement the following method, returning true
/// if it's known to be OK to use Swift reference-counting on values
/// of this type:
///   ReferenceCounting getReferenceCounting() const;
template <class Impl>
class HeapTypeInfo
    : public SingleScalarTypeInfoWithTypeLayout<Impl, ReferenceTypeInfo> {
  using super = SingleScalarTypeInfoWithTypeLayout<Impl, ReferenceTypeInfo>;

  llvm::Type *getOptionalIntType() const {
    return llvm::IntegerType::get(this->getStorageType()->getContext(),
                                  this->getFixedSize().getValueInBits());
  }

protected:
  using super::asDerived;
public:
  HeapTypeInfo(ReferenceCounting refcounting, llvm::PointerType *storage,
               Size size, SpareBitVector spareBits, Alignment align)
      : super(refcounting == ReferenceCounting::Native
                  ? ScalarKind::NativeStrongReference
              : refcounting == ReferenceCounting::Bridge
                  ? ScalarKind::BridgeReference
              : refcounting == ReferenceCounting::Block
                  ? ScalarKind::BlockReference
              : refcounting == ReferenceCounting::Error
                  ? ScalarKind::ErrorReference
              : refcounting == ReferenceCounting::Unknown
                  ? ScalarKind::UnknownReference
                  : ScalarKind::ObjCReference,
              storage, size, spareBits, align) {}

  bool isSingleRetainablePointer(ResilienceExpansion expansion,
                                 ReferenceCounting *refcounting) const override {
    if (refcounting)
      *refcounting = asDerived().getReferenceCounting();
    return true;
  }
  
  bool canValueWitnessExtraInhabitantsUpTo(IRGenModule &IGM,
                                           unsigned index) const override {
    // Refcounting functions are no-ops when passed a null pointer, which is the
    // first extra inhabitant.
    return index == 0;
  }
  
  IsaEncoding getIsaEncoding(ResilienceExpansion expansion) const {
    switch (asDerived().getReferenceCounting()) {
    // We can access the isa of pure Swift heap objects directly.
    case ReferenceCounting::Native:
      return IsaEncoding::Pointer;
    // Use the ObjC runtime to access ObjC or mixed-heritage isas.
    case ReferenceCounting::ObjC:
    case ReferenceCounting::Block:
      return IsaEncoding::ObjC;
    case ReferenceCounting::Unknown:
      return IsaEncoding::Unknown;
    case ReferenceCounting::Error:
      llvm_unreachable("errortype doesn't have an isa");
    }
  }

  static const bool IsScalarPOD = false;

  // Emit the copy/destroy operations required by SingleScalarTypeInfo
  // using strong reference counting.
  void emitScalarRelease(IRGenFunction &IGF, llvm::Value *value,
                         Atomicity atomicity) const {
    IGF.emitStrongRelease(value, asDerived().getReferenceCounting(), atomicity);
  }

  void emitScalarFixLifetime(IRGenFunction &IGF, llvm::Value *value) const {
    return IGF.emitFixLifetime(value);
  }

  void emitScalarRetain(IRGenFunction &IGF, llvm::Value *value,
                        Atomicity atomicity) const {
    IGF.emitStrongRetain(value, asDerived().getReferenceCounting(), atomicity);
  }

  // Implement the primary retain/release operations of ReferenceTypeInfo
  // using basic reference counting.
  void strongRetain(IRGenFunction &IGF, Explosion &e,
                    Atomicity atomicity) const override {
    llvm::Value *value = e.claimNext();
    asDerived().emitScalarRetain(IGF, value, atomicity);
  }

  void strongRelease(IRGenFunction &IGF, Explosion &e,
                     Atomicity atomicity) const override {
    llvm::Value *value = e.claimNext();
    asDerived().emitScalarRelease(IGF, value, atomicity);
  }

#define REF_STORAGE_HELPER(Name) \
  const TypeInfo * \
  create##Name##StorageType(TypeConverter &TC, \
                            bool isOptional) const override { \
    return TC.create##Name##StorageType(this->getStorageType(), \
                                        asDerived().getReferenceCounting(), \
                                        isOptional); \
  }
#define NEVER_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name, name) \
  void name##LoadStrong(IRGenFunction &IGF, Address src, \
                         Explosion &out, bool isOptional) const override { \
    llvm::Value *value = IGF.emit##Name##LoadStrong(src, \
                                          this->getStorageType(), \
                                          asDerived().getReferenceCounting()); \
    if (isOptional) { \
      out.add(IGF.Builder.CreatePtrToInt(value, getOptionalIntType())); \
    } else { \
      out.add(value); \
    } \
  } \
  void name##TakeStrong(IRGenFunction &IGF, Address src, \
                         Explosion &out, bool isOptional) const override { \
    llvm::Value *value = IGF.emit##Name##TakeStrong(src, \
                                          this->getStorageType(), \
                                          asDerived().getReferenceCounting()); \
    if (isOptional) { \
      out.add(IGF.Builder.CreatePtrToInt(value, getOptionalIntType())); \
    } else { \
      out.add(value); \
    } \
  } \
  void name##Init(IRGenFunction &IGF, Explosion &in, \
                  Address dest, bool isOptional) const override { \
    llvm::Value *value = in.claimNext(); \
    if (isOptional) { \
      assert(value->getType() == getOptionalIntType()); \
      value = IGF.Builder.CreateIntToPtr(value, this->getStorageType()); \
    } \
    IGF.emit##Name##Init(value, dest, asDerived().getReferenceCounting()); \
  } \
  void name##Assign(IRGenFunction &IGF, Explosion &in, \
                    Address dest, bool isOptional) const override { \
    llvm::Value *value = in.claimNext(); \
    if (isOptional) { \
      assert(value->getType() == getOptionalIntType()); \
      value = IGF.Builder.CreateIntToPtr(value, this->getStorageType()); \
    } \
    IGF.emit##Name##Assign(value, dest, asDerived().getReferenceCounting()); \
  }
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name, name) \
  void strongRetain##Name(IRGenFunction &IGF, Explosion &e, \
                          Atomicity atomicity) const override { \
    llvm::Value *value = e.claimNext(); \
    assert(asDerived().getReferenceCounting() == ReferenceCounting::Native); \
    IGF.emitNativeStrongRetain##Name(value, atomicity); \
  } \
  void strongRetain##Name##Release(IRGenFunction &IGF, Explosion &e, \
                                   Atomicity atomicity) const override { \
    llvm::Value *value = e.claimNext(); \
    assert(asDerived().getReferenceCounting() == ReferenceCounting::Native); \
    IGF.emitNativeStrongRetainAnd##Name##Release(value, atomicity); \
  } \
  void name##Retain(IRGenFunction &IGF, Explosion &e, \
                    Atomicity atomicity) const override { \
    llvm::Value *value = e.claimNext(); \
    assert(asDerived().getReferenceCounting() == ReferenceCounting::Native); \
    IGF.emitNative##Name##Retain(value, atomicity); \
  } \
  void name##Release(IRGenFunction &IGF, Explosion &e, \
                      Atomicity atomicity) const override { \
    llvm::Value *value = e.claimNext(); \
    assert(asDerived().getReferenceCounting() == ReferenceCounting::Native); \
    IGF.emitNative##Name##Release(value, atomicity); \
  }
#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...) \
  NEVER_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name, name) \
  REF_STORAGE_HELPER(Name)
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...) \
  ALWAYS_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name, name) \
  REF_STORAGE_HELPER(Name)
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...) \
  NEVER_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name, name) \
  ALWAYS_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name, name) \
  REF_STORAGE_HELPER(Name)
#define UNCHECKED_REF_STORAGE(Name, ...) \
  REF_STORAGE_HELPER(Name)
#include "swift/AST/ReferenceStorage.def"
#undef REF_STORAGE_HELPER
#undef NEVER_LOADABLE_CHECKED_REF_STORAGE_HELPER
#undef ALWAYS_LOADABLE_CHECKED_REF_STORAGE_HELPER

  LoadedRef loadRefcountedPtr(IRGenFunction &IGF, SourceLoc loc,
                              Address addr) const override {
    llvm::Value *ptr =
      IGF.emitLoadRefcountedPtr(addr, asDerived().getReferenceCounting());
    return LoadedRef(ptr, true);
  }

  ReferenceCounting getReferenceCountingType() const override {
    return asDerived().getReferenceCounting();
  }

  // Extra inhabitants of heap object pointers.

  bool mayHaveExtraInhabitants(IRGenModule &IGM) const override {
    return true;
  }

  unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const override {
    return getHeapObjectExtraInhabitantCount(IGM);
  }

  APInt getFixedExtraInhabitantValue(IRGenModule &IGM,
                                     unsigned bits,
                                     unsigned index) const override {
    return getHeapObjectFixedExtraInhabitantValue(IGM, bits, index, 0);
  }

  llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF, Address src,
                                       SILType T, bool isOutlined)
  const override {
    return getHeapObjectExtraInhabitantIndex(IGF, src);
  }

  void storeExtraInhabitant(IRGenFunction &IGF, llvm::Value *index,
                            Address dest, SILType T, bool isOutlined)
  const override {
    return storeHeapObjectExtraInhabitant(IGF, index, dest);
  }
};

}
}

#endif

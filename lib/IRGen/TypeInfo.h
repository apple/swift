//===--- TypeInfo.h - Abstract primitive operations on values ---*- C++ -*-===//
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
// This file defines the interface used to perform primitive
// operations on swift values and objects.
//
// This interface is supplemented in two ways:
//   - FixedTypeInfo provides a number of operations meaningful only
//     for types with a fixed-size representation
//   - ReferenceTypeInfo is a further refinement of FixedTypeInfo
//     which provides operations meaningful only for types with
//     reference semantics
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_TYPEINFO_H
#define SWIFT_IRGEN_TYPEINFO_H

#include "IRGen.h"
#include "swift/AST/ReferenceCounting.h"
#include "llvm/ADT/MapVector.h"

namespace llvm {
  class Constant;
  class Twine;
  class Type;
}

namespace swift {
  enum IsInitialization_t : bool;
  enum IsTake_t : bool;
  class SILType;

namespace irgen {
  class Address;
  class StackAddress;
  class IRGenFunction;
  class IRGenTypeVerifierFunction;
  class IRGenModule;
  class Explosion;
  class ExplosionSchema;
  class NativeConventionSchema;
  enum OnHeap_t : unsigned char;
  class OutliningMetadataCollector;
  class OwnedAddress;
  class RValue;
  class RValueSchema;
  class TypeLayoutEntry;

/// Ways in which an object can fit into a fixed-size buffer.
enum class FixedPacking {
  /// It fits at offset zero.
  OffsetZero,
  
  /// It doesn't fit and needs to be side-allocated.
  Allocate,
  
  /// It needs to be checked dynamically.
  Dynamic
};

enum class SpecialTypeInfoKind : uint8_t {
  Unimplemented,

  None,

  /// Everything after this is statically fixed-size.
  Fixed,
  Weak,

  /// Everything after this is loadable.
  Loadable,
  Reference,

  Last_Kind = Reference
};
enum : unsigned { NumSpecialTypeInfoKindBits =
  countBitsUsed(static_cast<unsigned>(SpecialTypeInfoKind::Last_Kind)) };

/// Information about the IR representation and generation of the
/// given type.
class TypeInfo {
  TypeInfo(const TypeInfo &) = delete;
  TypeInfo &operator=(const TypeInfo &) = delete;

  friend class TypeConverter;

protected:
  // clang-format off
  union {
    uint64_t OpaqueBits;

    SWIFT_INLINE_BITFIELD_BASE(TypeInfo,
                             bitmax(NumSpecialTypeInfoKindBits,8)+6+1+1+1+3+1+1,
      /// The kind of supplemental API this type has, if any.
      Kind : bitmax(NumSpecialTypeInfoKindBits,8),

      /// The storage alignment of this type in log2 bytes.
      AlignmentShift : 6,

      /// Whether this type is known to be trivially destructible.
      TriviallyDestroyable : 1,
      
      /// Whether this type is known to be bitwise-takable.
      BitwiseTakable : 1,

      /// Whether this type is known to be copyable.
      Copyable : 1,

      /// An arbitrary discriminator for the subclass.  This is useful for e.g.
      /// distinguishing between different TypeInfos that all implement the same
      /// kind of type.
      /// FIXME -- Create TypeInfoNodes.def and get rid of this field.
      SubclassKind : 3,

      /// Whether this type can be assumed to have a fixed size from all
      /// resilience domains.
      AlwaysFixedSize : 1,

      /// Whether this type is ABI-accessible from this SILModule.
      ABIAccessible : 1
    );

    /// FixedTypeInfo will use the remaining bits for the size.
    ///
    /// NOTE: Until one can define statically sized inline arrays in the
    /// language, defining an extremely large object is quite impractical.
    /// For now: "4 GiB should be more than good enough."
    SWIFT_INLINE_BITFIELD_FULL(FixedTypeInfo, TypeInfo, 32,
      : NumPadBits,

      /// The storage size of this type in bytes.  This may be zero even
      /// for well-formed and complete types, such as a trivial enum or
      /// tuple.
      Size : 32
    );
  } Bits;
  // clang-format on

  enum { InvalidSubclassKind = 0x7 };

  TypeInfo(llvm::Type *Type, Alignment A, IsTriviallyDestroyable_t pod,
           IsBitwiseTakable_t bitwiseTakable,
           IsCopyable_t copyable,
           IsFixedSize_t alwaysFixedSize,
           IsABIAccessible_t abiAccessible,
           SpecialTypeInfoKind stik) : StorageType(Type) {
    assert(stik >= SpecialTypeInfoKind::Fixed || !alwaysFixedSize);
    Bits.OpaqueBits = 0;
    Bits.TypeInfo.Kind = unsigned(stik);
    Bits.TypeInfo.AlignmentShift = llvm::Log2_32(A.getValue());
    Bits.TypeInfo.TriviallyDestroyable = pod;
    Bits.TypeInfo.BitwiseTakable = bitwiseTakable;
    Bits.TypeInfo.Copyable = copyable;
    Bits.TypeInfo.SubclassKind = InvalidSubclassKind;
    Bits.TypeInfo.AlwaysFixedSize = alwaysFixedSize;
    Bits.TypeInfo.ABIAccessible = abiAccessible;
  }

  /// Change the minimum alignment of a stored value of this type.
  void setStorageAlignment(Alignment alignment) {
    auto Prev = Bits.TypeInfo.AlignmentShift;
    auto Next = llvm::Log2_32(alignment.getValue());
    assert(Next >= Prev && "Alignment can only increase");
    (void)Prev;
    Bits.TypeInfo.AlignmentShift = Next;
  }

  void setSubclassKind(unsigned kind) {
    assert(kind != InvalidSubclassKind);
    Bits.TypeInfo.SubclassKind = kind;
    assert(Bits.TypeInfo.SubclassKind == kind && "kind was truncated?");
  }

private:
  mutable const TypeInfo *NextConverted = nullptr;

  /// The LLVM representation of a stored value of this type.  For
  /// non-fixed types, this is really useful only for forming pointers to it.
  llvm::Type *StorageType;

  mutable NativeConventionSchema *nativeReturnSchema = nullptr;
  mutable NativeConventionSchema *nativeParameterSchema = nullptr;

public:
  virtual ~TypeInfo();

  /// Unsafely cast this to the given subtype.
  template <class T> const T &as() const {
    // FIXME: maybe do an assert somehow if we have RTTI enabled.
    return static_cast<const T &>(*this);
  }

  /// Whether this type is known to be empty.
  bool isKnownEmpty(ResilienceExpansion expansion) const;

  /// Whether this type is known to be ABI-accessible, i.e. whether it's
  /// actually possible to do ABI operations on it from this current SILModule.
  /// See SILModule::isTypeABIAccessible.
  ///
  /// All fixed-size types are currently ABI-accessible, although this would
  /// not be difficult to change (e.g. if we had an archetype size constraint
  /// that didn't say anything about triviality).
  IsABIAccessible_t isABIAccessible() const {
    return IsABIAccessible_t(Bits.TypeInfo.ABIAccessible);
  }

  /// Whether this type is known to be trivially destroyable, i.e. to not
  /// require any particular action on destroy. If the type is also copyable,
  /// this implies that copying is also bitwise, i.e., equivalent to memcpy.
  IsTriviallyDestroyable_t isTriviallyDestroyable(ResilienceExpansion expansion) const {
    return IsTriviallyDestroyable_t(Bits.TypeInfo.TriviallyDestroyable);
  }
  
  /// Whether this type is known to be copyable.
  IsCopyable_t isCopyable(ResilienceExpansion expansion) const {
    return IsCopyable_t(Bits.TypeInfo.Copyable);
  }
  
  /// Whether this type is known to be bitwise-takable, i.e. "initializeWithTake"
  /// is equivalent to a memcpy.
  IsBitwiseTakable_t isBitwiseTakable(ResilienceExpansion expansion) const {
    return IsBitwiseTakable_t(Bits.TypeInfo.BitwiseTakable);
  }
  
  /// Returns the type of special interface followed by this TypeInfo.
  /// It is important for our design that this depends only on
  /// immediate type structure and not on, say, properties that can
  /// vary by resilience.  Of course, generics can obscure these
  /// properties on their parameter types, but then the program
  /// can rely on them.
  SpecialTypeInfoKind getSpecialTypeInfoKind() const {
    return SpecialTypeInfoKind(Bits.TypeInfo.Kind);
  }

  /// Returns whatever arbitrary data has been stash in the subclass
  /// kind field.  This mechanism allows an orthogonal dimension of
  /// distinguishing between TypeInfos, which is useful when multiple
  /// TypeInfo subclasses are used to implement the same kind of type.
  unsigned getSubclassKind() const {
    assert(Bits.TypeInfo.SubclassKind != InvalidSubclassKind &&
           "subclass kind has not been initialized!");
    return Bits.TypeInfo.SubclassKind;
  }

  /// Whether this type is known to be fixed-size in the local
  /// resilience domain.  If true, this TypeInfo can be cast to
  /// FixedTypeInfo.
  IsFixedSize_t isFixedSize() const {
    return IsFixedSize_t(getSpecialTypeInfoKind() >= SpecialTypeInfoKind::Fixed);
  }

  /// Whether this type is known to be fixed-size in the given
  /// resilience domain.  If true, spare bits can be used.
  IsFixedSize_t isFixedSize(ResilienceExpansion expansion) const {
    switch (expansion) {
    case ResilienceExpansion::Maximal:
      return isFixedSize();
    case ResilienceExpansion::Minimal:
      // We can't be universally fixed size if we're not locally
      // fixed size.
      assert((isFixedSize() || Bits.TypeInfo.AlwaysFixedSize == IsNotFixedSize) &&
             "IsFixedSize vs IsAlwaysFixedSize mismatch");
      return IsFixedSize_t(Bits.TypeInfo.AlwaysFixedSize);
    }

    llvm_unreachable("Not a valid ResilienceExpansion.");
  }

  /// Whether this type is known to be loadable in the local
  /// resilience domain.  If true, this TypeInfo can be cast to
  /// LoadableTypeInfo.
  IsLoadable_t isLoadable() const {
    return IsLoadable_t(getSpecialTypeInfoKind() >= SpecialTypeInfoKind::Loadable);
  }

  llvm::Type *getStorageType() const { return StorageType; }

  Alignment getBestKnownAlignment() const {
    auto Shift = Bits.TypeInfo.AlignmentShift;
    return Alignment(1ull << Shift);
  }

  /// Given a generic pointer to this type, produce an Address for it.
  Address getAddressForPointer(llvm::Value *ptr) const;

  /// Produce an undefined pointer to an object of this type.
  Address getUndefAddress() const;
    
  /// Return the size and alignment of this type.
  virtual llvm::Value *getSize(IRGenFunction &IGF, SILType T) const = 0;
  virtual llvm::Value *getAlignmentMask(IRGenFunction &IGF, SILType T) const = 0;
  virtual llvm::Value *getStride(IRGenFunction &IGF, SILType T) const = 0;
  virtual llvm::Value *getIsTriviallyDestroyable(IRGenFunction &IGF, SILType T) const = 0;
  virtual llvm::Value *getIsBitwiseTakable(IRGenFunction &IGF, SILType T) const = 0;
  virtual llvm::Value *isDynamicallyPackedInline(IRGenFunction &IGF,
                                                 SILType T) const = 0;

  /// Return the statically-known size of this type, or null if it is
  /// not known.
  virtual llvm::Constant *getStaticSize(IRGenModule &IGM) const = 0;

  /// Return the statically-known alignment mask for this type, or
  /// null if it is not known.
  virtual llvm::Constant *getStaticAlignmentMask(IRGenModule &IGM) const = 0;

  /// Return the statically-known stride size of this type, or null if
  /// it is not known.
  virtual llvm::Constant *getStaticStride(IRGenModule &IGM) const = 0;

  /// Add the information for exploding values of this type to the
  /// given schema.
  virtual void getSchema(ExplosionSchema &schema) const = 0;

  /// A convenience for getting the schema of a single type.
  ExplosionSchema getSchema() const;

  /// Build the type layout for this type info.
  virtual TypeLayoutEntry
  *buildTypeLayoutEntry(IRGenModule &IGM,
                        SILType T,
                        bool useStructLayouts) const = 0;

  /// Allocate a variable of this type on the stack.
  virtual StackAddress allocateStack(IRGenFunction &IGF, SILType T,
                                     const llvm::Twine &name) const = 0;

  /// Deallocate a variable of this type.
  virtual void deallocateStack(IRGenFunction &IGF, StackAddress addr,
                               SILType T) const = 0;

  /// Destroy the value of a variable of this type, then deallocate its
  /// memory.
  virtual void destroyStack(IRGenFunction &IGF, StackAddress addr, SILType T,
                            bool isOutlined) const = 0;

  /// Copy or take a value out of one address and into another, destroying
  /// old value in the destination.  Equivalent to either assignWithCopy
  /// or assignWithTake depending on the value of isTake.
  void assign(IRGenFunction &IGF, Address dest, Address src, IsTake_t isTake,
              SILType T, bool isOutlined) const;

  /// Copy a value out of an object and into another, destroying the
  /// old value in the destination.
  virtual void assignWithCopy(IRGenFunction &IGF, Address dest, Address src,
                              SILType T, bool isOutlined) const = 0;

  /// Move a value out of an object and into another, destroying the
  /// old value there and leaving the source object in an invalid state.
  virtual void assignWithTake(IRGenFunction &IGF, Address dest, Address src,
                              SILType T, bool isOutlined) const = 0;

  /// Copy-initialize or take-initialize an uninitialized object
  /// with the value from a different object.  Equivalent to either
  /// initializeWithCopy or initializeWithTake depending on the value
  /// of isTake.
  void initialize(IRGenFunction &IGF, Address dest, Address src,
                  IsTake_t isTake, SILType T, bool isOutlined) const;

  /// Perform a "take-initialization" from the given object.  A
  /// take-initialization is like a C++ move-initialization, except that
  /// the old object is actually no longer permitted to be destroyed.
  virtual void initializeWithTake(IRGenFunction &IGF, Address destAddr,
                                  Address srcAddr, SILType T,
                                  bool isOutlined) const = 0;

  /// Perform a copy-initialization from the given object.
  virtual void initializeWithCopy(IRGenFunction &IGF, Address destAddr,
                                  Address srcAddr, SILType T,
                                  bool isOutlined) const = 0;

  /// Perform a copy-initialization from the given fixed-size buffer
  /// into an uninitialized fixed-size buffer, allocating the buffer if
  /// necessary.  Returns the address of the value inside the buffer.
  ///
  /// This is equivalent to:
  ///   auto srcAddress = projectBuffer(IGF, srcBuffer, T);
  ///   initializeBufferWithCopy(IGF, destBuffer, srcAddress, T);
  /// but will be more efficient for dynamic types, since it uses a single
  /// value witness call.
  virtual Address initializeBufferWithCopyOfBuffer(IRGenFunction &IGF,
                                                   Address destBuffer,
                                                   Address srcBuffer,
                                                   SILType T) const;

  /// Take-initialize an address from a parameter explosion.
  virtual void initializeFromParams(IRGenFunction &IGF, Explosion &params,
                                    Address src, SILType T,
                                    bool isOutlined) const = 0;

  /// Destroy an object of this type in memory.
  virtual void destroy(IRGenFunction &IGF, Address address, SILType T,
                       bool isOutlined) const = 0;

  /// Should optimizations be enabled which rely on the representation
  /// for this type being a single object pointer?
  ///
  /// \return false by default
  virtual bool isSingleRetainablePointer(ResilienceExpansion expansion,
                                         ReferenceCounting *refcounting
                                             = nullptr) const;

  /// Should optimizations be enabled which rely on the representation
  /// for this type being a single Swift-retainable object pointer?
  ///
  /// \return false by default
  bool isSingleSwiftRetainablePointer(ResilienceExpansion expansion) const {
    ReferenceCounting refcounting;
    return (isSingleRetainablePointer(expansion, &refcounting) &&
            refcounting == ReferenceCounting::Native);
  }

  /// Does this type statically have extra inhabitants, or may it dynamically
  /// have extra inhabitants based on type arguments?
  virtual bool mayHaveExtraInhabitants(IRGenModule &IGM) const = 0;

  /// Returns true if the value witness operations on this type work correctly
  /// with extra inhabitants up to the given index.
  ///
  /// An example of this is retainable pointers. The first extra inhabitant for
  /// these types is the null pointer, on which swift_retain is a harmless
  /// no-op. If this predicate returns true, then a single-payload enum with
  /// this type as its payload (like Optional<T>) can avoid additional branching
  /// on the enum tag for value witness operations.
  virtual bool canValueWitnessExtraInhabitantsUpTo(IRGenModule &IGM,
                                                   unsigned index) const;
  
  /// Get the tag of a single payload enum with a payload of this type (\p T) e.g
  /// Optional<T>.
  virtual llvm::Value *getEnumTagSinglePayload(IRGenFunction &IGF,
                                               llvm::Value *numEmptyCases,
                                               Address enumAddr,
                                               SILType T,
                                               bool isOutlined) const = 0;

  /// Store the tag of a single payload enum with a payload of this type.
  virtual void storeEnumTagSinglePayload(IRGenFunction &IGF,
                                         llvm::Value *whichCase,
                                         llvm::Value *numEmptyCases,
                                         Address enumAddr,
                                         SILType T,
                                         bool isOutlined) const = 0;

  /// Return an extra-inhabitant tag for the given type, which will be
  /// 0 for a value that's not an extra inhabitant or else a value in
  /// 1...extraInhabitantCount.  Note that the range is off by one relative
  /// to the expectations of FixedTypeInfo::getExtraInhabitantIndex!
  ///
  /// Most places in IRGen shouldn't be using this.
  ///
  /// knownXICount can be null.
  llvm::Value *getExtraInhabitantTagDynamic(IRGenFunction &IGF,
                                            Address address,
                                            SILType T,
                                            llvm::Value *knownXICount,
                                            bool isOutlined) const;

  /// Store an extra-inhabitant tag for the given type, which is known to be
  /// in 1...extraInhabitantCount.  Note that the range is off by one
  /// relative to the expectations of FixedTypeInfo::storeExtraInhabitant!
  ///
  /// Most places in IRGen shouldn't be using this.
  void storeExtraInhabitantTagDynamic(IRGenFunction &IGF,
                                      llvm::Value *index,
                                      Address address,
                                      SILType T,
                                      bool isOutlined) const;

  /// Compute the packing of values of this type into a fixed-size buffer.
  /// A value might not be stored in the fixed-size buffer because it does not
  /// fit or because it is not bit-wise takable. Non bit-wise takable values are
  /// not stored inline by convention.
  FixedPacking getFixedPacking(IRGenModule &IGM) const;
  
  /// Index into an array of objects of this type.
  Address indexArray(IRGenFunction &IGF, Address base, llvm::Value *offset,
                     SILType T) const;

  /// Round up the address value \p base to the alignment of type \p T. 
  Address roundUpToTypeAlignment(IRGenFunction &IGF, Address base,
                                 SILType T) const;

  /// Destroy an array of objects of this type in memory.
  virtual void destroyArray(IRGenFunction &IGF, Address base,
                            llvm::Value *count, SILType T) const;
  
  /// Initialize an array of objects of this type in memory by copying the
  /// values from another array. The arrays must not overlap.
  virtual void initializeArrayWithCopy(IRGenFunction &IGF,
                                       Address dest,
                                       Address src,
                                       llvm::Value *count, SILType T) const;
  
  /// Initialize an array of objects of this type in memory by taking the
  /// values from another array. The array must not overlap.
  virtual void initializeArrayWithTakeNoAlias(IRGenFunction &IGF,
                                       Address dest, Address src,
                                       llvm::Value *count, SILType T) const;

  /// Initialize an array of objects of this type in memory by taking the
  /// values from another array. The destination array may overlap the head of
  /// the source array because the elements are taken as if in front-to-back
  /// order.
  virtual void initializeArrayWithTakeFrontToBack(IRGenFunction &IGF,
                                       Address dest, Address src,
                                       llvm::Value *count, SILType T) const;
  
  /// Initialize an array of objects of this type in memory by taking the
  /// values from another array. The destination array may overlap the tail of
  /// the source array because the elements are taken as if in back-to-front
  /// order.
  virtual void initializeArrayWithTakeBackToFront(IRGenFunction &IGF,
                                       Address dest, Address src,
                                       llvm::Value *count, SILType T) const;

  /// Assign to an array of objects of this type in memory by copying the
  /// values from another array. The array must not overlap.
  virtual void assignArrayWithCopyNoAlias(IRGenFunction &IGF, Address dest,
                                          Address src, llvm::Value *count,
                                          SILType T) const;

  /// Assign to an array of objects of this type in memory by copying the
  /// values from another array. The destination array may overlap the head of
  /// the source array because the elements are taken as if in front-to-back
  /// order.
  virtual void assignArrayWithCopyFrontToBack(IRGenFunction &IGF, Address dest,
                                              Address src, llvm::Value *count,
                                              SILType T) const;

  /// Assign to an array of objects of this type in memory by copying the
  /// values from another array. The destination array may overlap the tail of
  /// the source array because the elements are taken as if in back-to-front
  /// order.
  virtual void assignArrayWithCopyBackToFront(IRGenFunction &IGF, Address dest,
                                              Address src, llvm::Value *count,
                                              SILType T) const;

  /// Assign to an array of objects of this type in memory by taking the
  /// values from another array. The array must not overlap.
  virtual void assignArrayWithTake(IRGenFunction &IGF, Address dest,
                                   Address src, llvm::Value *count,
                                   SILType T) const;

  /// Collect all the metadata necessary in order to perform value
  /// operations on this type.
  virtual void collectMetadataForOutlining(OutliningMetadataCollector &collector,
                                           SILType T) const;

  /// Get the native (abi) convention for a return value of this type.
  const NativeConventionSchema &nativeReturnValueSchema(IRGenModule &IGM) const;

  /// Get the native (abi) convention for a parameter value of this type.
  const NativeConventionSchema &nativeParameterValueSchema(IRGenModule &IGM) const;
  
  /// Emit verifier code that compares compile-time constant knowledge of
  /// this kind of type's traits to its runtime manifestation.
  virtual void verify(IRGenTypeVerifierFunction &IGF,
                      llvm::Value *typeMetadata,
                      SILType T) const;

  void callOutlinedCopy(IRGenFunction &IGF, Address dest, Address src,
                        SILType T, IsInitialization_t isInit,
                        IsTake_t isTake) const;

  void callOutlinedDestroy(IRGenFunction &IGF, Address addr, SILType T) const;
};

} // end namespace irgen
} // end namespace swift

#endif

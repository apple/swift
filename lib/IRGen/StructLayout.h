//===--- StructLayout.h - Structure layout ----------------------*- C++ -*-===//
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
// This file defines some routines that are useful for performing
// structure layout.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_STRUCTLAYOUT_H
#define SWIFT_IRGEN_STRUCTLAYOUT_H

#include "llvm/ADT/ArrayRef.h"
#include "swift/Basic/ClusteredBitVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "IRGen.h"

namespace llvm {
  class Constant;
  class StructType;
  class Type;
  class Value;
}

namespace swift {
namespace irgen {
  class Address;
  class IRGenFunction;
  class IRGenModule;
  class TypeInfo;

/// An algorithm for laying out a structure.
enum class LayoutStrategy {
  /// Compute an optimal layout;  there are no constraints at all.
  Optimal,

  /// The 'universal' strategy: all modules must agree on the layout.
  Universal
};

/// The kind of object being laid out.
enum class LayoutKind {
  /// A non-heap object does not require a heap header.
  NonHeapObject,

  /// A heap object is destined to be allocated on the heap and must
  /// be emitted with the standard heap header.
  HeapObject,
};

class NonFixedOffsetsImpl;

/// The type to pass around for non-fixed offsets.
using NonFixedOffsets = llvm::Optional<NonFixedOffsetsImpl *>;

/// An abstract class for determining non-fixed offsets.
class NonFixedOffsetsImpl {
protected:
  virtual ~NonFixedOffsetsImpl() = default;
public:
  /// Return the offset (in bytes, as a size_t) of the element with
  /// the given index.
  virtual llvm::Value *getOffsetForIndex(IRGenFunction &IGF,
                                         unsigned index) = 0;

  operator NonFixedOffsets() { return NonFixedOffsets(this); }
};

/// An element layout is the layout for a single element of some sort
/// of aggregate structure.
class ElementLayout {
public:
  enum class Kind {
    /// The element is known to require no storage in the aggregate.
    /// Its offset in the aggregate is always statically zero.
    Empty,

    /// The element is known to require no storage in the aggregate.
    /// But it has an offset in the aggregate. This is to support getting the
    /// offset of tail allocated storage using MemoryLayout<>.offset(of:).
    EmptyTailAllocatedCType,

    /// The element can be positioned at a fixed offset within the
    /// aggregate.
    Fixed,

    /// The element cannot be positioned at a fixed offset within the
    /// aggregate.
    NonFixed,

    /// The element is an object lacking a fixed size but located at
    /// offset zero.  This is necessary because LLVM forbids even a
    /// 'gep 0' on an unsized type.
    InitialNonFixedSize

    // IncompleteKind comes here
  };

private:
  enum : unsigned { IncompleteKind  = unsigned(Kind::InitialNonFixedSize) + 1 };

  /// The swift type information for this element's layout.
  const TypeInfo *Type;

  /// The offset in bytes from the start of the struct.
  unsigned ByteOffset;

  /// The offset in bytes from the start of the struct, except EmptyFields are
  /// placed at the current byte offset instead of 0. For the purpose of the
  /// final layout empty fields are placed at offset 0, that however creates a
  /// whole slew of special cases to deal with. Instead of dealing with these
  /// special cases during layout, we pretend that empty fields are placed
  /// just like any other field at the current offset.
  unsigned ByteOffsetForLayout;

  /// The index of this element, either in the LLVM struct (if fixed)
  /// or in the non-fixed elements array (if non-fixed).
  unsigned Index : 28;

  /// Whether this element is known to be trivially destructible in the local
  /// resilience domain.
  unsigned IsTriviallyDestroyable : 1;

  /// The kind of layout performed for this element.
  unsigned TheKind : 3;

  explicit ElementLayout(const TypeInfo &type)
    : Type(&type), TheKind(IncompleteKind) {}

  bool isCompleted() const {
    return (TheKind != IncompleteKind);
  }

public:
  static ElementLayout getIncomplete(const TypeInfo &type) {
    return ElementLayout(type);
  }

  void completeFrom(const ElementLayout &other) {
    assert(!isCompleted());
    TheKind = other.TheKind;
    IsTriviallyDestroyable = other.IsTriviallyDestroyable;
    ByteOffset = other.ByteOffset;
    ByteOffsetForLayout = other.ByteOffsetForLayout;
    Index = other.Index;
  }

  void completeEmpty(IsTriviallyDestroyable_t isTriviallyDestroyable, Size byteOffset) {
    TheKind = unsigned(Kind::Empty);
    IsTriviallyDestroyable = unsigned(isTriviallyDestroyable);
    ByteOffset = 0;
    ByteOffsetForLayout = byteOffset.getValue();
    Index = 0; // make a complete write of the bitfield
  }

  void completeInitialNonFixedSize(IsTriviallyDestroyable_t isTriviallyDestroyable) {
    TheKind = unsigned(Kind::InitialNonFixedSize);
    IsTriviallyDestroyable = unsigned(isTriviallyDestroyable);
    ByteOffset = 0;
    ByteOffsetForLayout = ByteOffset;
    Index = 0; // make a complete write of the bitfield
  }

  void completeFixed(IsTriviallyDestroyable_t isTriviallyDestroyable, Size byteOffset, unsigned structIndex) {
    TheKind = unsigned(Kind::Fixed);
    IsTriviallyDestroyable = unsigned(isTriviallyDestroyable);
    ByteOffset = byteOffset.getValue();
    ByteOffsetForLayout = ByteOffset;
    Index = structIndex;

    assert(getByteOffset() == byteOffset);
  }

  void completeEmptyTailAllocatedCType(IsTriviallyDestroyable_t isTriviallyDestroyable, Size byteOffset) {
    TheKind = unsigned(Kind::EmptyTailAllocatedCType);
    IsTriviallyDestroyable = unsigned(isTriviallyDestroyable);
    ByteOffset = byteOffset.getValue();
    ByteOffsetForLayout = ByteOffset;
    Index = 0;

    assert(getByteOffset() == byteOffset);
  }

  /// Complete this element layout with a non-fixed offset.
  ///
  /// \param nonFixedElementIndex - the index into the elements array
  void completeNonFixed(IsTriviallyDestroyable_t isTriviallyDestroyable, unsigned nonFixedElementIndex) {
    TheKind = unsigned(Kind::NonFixed);
    IsTriviallyDestroyable = unsigned(isTriviallyDestroyable);
    Index = nonFixedElementIndex;
  }

  const TypeInfo &getType() const { return *Type; }

  Kind getKind() const {
    assert(isCompleted());
    return Kind(TheKind);
  }

  /// Is this element known to be empty?
  bool isEmpty() const {
    return getKind() == Kind::Empty ||
           getKind() == Kind::EmptyTailAllocatedCType;
  }

  /// Is this element known to be POD?
  IsTriviallyDestroyable_t isTriviallyDestroyable() const {
    assert(isCompleted());
    return IsTriviallyDestroyable_t(IsTriviallyDestroyable);
  }

  /// Can we access this element at a static offset?
  bool hasByteOffset() const {
    switch (getKind()) {
    case Kind::Empty:
    case Kind::EmptyTailAllocatedCType:
    case Kind::Fixed:
      return true;

    // FIXME: InitialNonFixedSize should go in the above, but I'm being
    // paranoid about changing behavior.
    case Kind::InitialNonFixedSize:
    case Kind::NonFixed:
      return false;
    }
    llvm_unreachable("bad kind");
  }

  /// Given that this element has a fixed offset, return that offset in bytes.
  Size getByteOffset() const {
    assert(isCompleted() && hasByteOffset());
    return Size(ByteOffset);
  }

  /// The offset in bytes from the start of the struct, except EmptyFields are
  /// placed at the current byte offset instead of 0. For the purpose of the
  /// final layout empty fields are placed at offset 0, that however creates a
  /// whole slew of special cases to deal with. Instead of dealing with these
  /// special cases during layout, we pretend that empty fields are placed
  /// just like any other field at the current offset.
  Size getByteOffsetDuringLayout() const {
    assert(isCompleted() && hasByteOffset());
    return Size(ByteOffsetForLayout);
  }

  /// Given that this element has a fixed offset, return the index in
  /// the LLVM struct.
  unsigned getStructIndex() const {
    assert(isCompleted() && getKind() == Kind::Fixed);
    return Index;
  }

  /// Given that this element does not have a fixed offset, return its
  /// index in the nonfixed-elements array.
  unsigned getNonFixedElementIndex() const {
    assert(isCompleted() && getKind() == Kind::NonFixed);
    return Index;
  }

  Address project(IRGenFunction &IGF, Address addr,
                  NonFixedOffsets offsets,
                  const llvm::Twine &suffix = "") const;
};

/// A class for building a structure layout.
class StructLayoutBuilder {
protected:
  IRGenModule &IGM;
  SmallVector<llvm::Type*, 8> StructFields;
  Size CurSize = Size(0);
  Size headerSize = Size(0);
private:
  Alignment CurAlignment = Alignment(1);
  SmallVector<SpareBitVector, 8> CurSpareBits;
  unsigned NextNonFixedOffsetIndex = 0;
  bool IsFixedLayout = true;
  IsTriviallyDestroyable_t IsKnownTriviallyDestroyable = IsTriviallyDestroyable;
  IsBitwiseTakable_t IsKnownBitwiseTakable = IsBitwiseTakable;
  IsCopyable_t IsKnownCopyable = IsCopyable;
  IsFixedSize_t IsKnownAlwaysFixedSize = IsFixedSize;
public:
  StructLayoutBuilder(IRGenModule &IGM) : IGM(IGM) {}

  /// Add a swift heap header to the layout.  This must be the first
  /// thing added to the layout.
  void addHeapHeader();
  /// Add the NSObject object header to the layout. This must be the first
  /// thing added to the layout.
  void addNSObjectHeader();
  /// Add the default-actor header to the layout.  This must be the second
  /// thing added to the layout, following the Swift heap header.
  void addDefaultActorHeader(ElementLayout &elt);
  /// Add the non-default distributed actor header to the layout.
  /// This must be the second thing added to the layout, following the Swift heap header.
  void addNonDefaultDistributedActorHeader(ElementLayout &elt);

  /// Add a number of fields to the layout.  The field layouts need
  /// only have the TypeInfo set; the rest will be filled out.
  ///
  /// Returns true if the fields may have increased the storage
  /// requirements of the layout.
  bool addFields(llvm::MutableArrayRef<ElementLayout> fields,
                 LayoutStrategy strategy);

  /// Add a field to the layout.  The field layout needs
  /// only have the TypeInfo set; the rest will be filled out.
  ///
  /// Returns true if the field may have increased the storage
  /// requirements of the layout.
  bool addField(ElementLayout &elt, LayoutStrategy strategy);

  /// Return whether the layout is known to be empty.
  bool empty() const { return IsFixedLayout && CurSize == Size(0); }

  /// Return the current set of fields.
  ArrayRef<llvm::Type *> getStructFields() const { return StructFields; }

  /// Return whether the structure has a fixed-size layout.
  bool isFixedLayout() const { return IsFixedLayout; }

  /// Return whether the structure is known to be POD in the local
  /// resilience scope.
  IsTriviallyDestroyable_t isTriviallyDestroyable() const { return IsKnownTriviallyDestroyable; }

  /// Return whether the structure is known to be bitwise-takable in the local
  /// resilience scope.
  IsBitwiseTakable_t isBitwiseTakable() const {
    return IsKnownBitwiseTakable;
  }
  
  /// Return whether the structure is known to be copyable in the local
  /// resilience scope.
  IsCopyable_t isCopyable() const {
    return IsKnownCopyable;
  }

  /// Return whether the structure is known to be fixed-size in all
  /// resilience scopes.
  IsFixedSize_t isAlwaysFixedSize() const {
    return IsKnownAlwaysFixedSize;
  }

  /// Return the size of the structure built so far.
  Size getSize() const { return CurSize; }

  // Return the size of the header.
  Size getHeaderSize() const { return headerSize; }

  /// Return the alignment of the structure built so far.
  Alignment getAlignment() const { return CurAlignment; }

  /// Return the spare bit mask of the structure built so far.
  SpareBitVector getSpareBits() const;

  /// Build the current elements as a new anonymous struct type.
  llvm::StructType *getAsAnonStruct() const;

  /// Build the current elements as a new anonymous struct type.
  void setAsBodyOfStruct(llvm::StructType *type) const;

private:
  void addFixedSizeElement(ElementLayout &elt);
  void addNonFixedSizeElement(ElementLayout &elt);
  void addEmptyElement(ElementLayout &elt);

  void addElementAtFixedOffset(ElementLayout &elt);
  void addElementAtNonFixedOffset(ElementLayout &elt);
  void addNonFixedSizeElementAtOffsetZero(ElementLayout &elt);
};

/// Apply layout attributes such as @_alignment to the layout properties of a
/// type, diagnosing any problems with them.
void applyLayoutAttributes(IRGenModule &IGM,
                           NominalTypeDecl *decl,
                           bool isFixedLayout,
                           /*inout*/ Alignment &alignment);

/// A struct layout is the result of laying out a complete structure.
class StructLayout {
  /// The statically-known minimum bound on the alignment.
  Alignment MinimumAlign;

  /// The statically-known minimum bound on the size.
  Size MinimumSize;

  /// The size of a header if present.
  Size headerSize;
  
  /// The statically-known spare bit mask.
  SpareBitVector SpareBits;

  /// Whether this layout is fixed in size.  If so, the size and
  /// alignment are exact.
  bool IsFixedLayout;

  IsTriviallyDestroyable_t IsKnownTriviallyDestroyable;
  IsBitwiseTakable_t IsKnownBitwiseTakable;
  IsCopyable_t IsKnownCopyable;
  IsFixedSize_t IsKnownAlwaysFixedSize = IsFixedSize;
  
  llvm::Type *Ty;
  SmallVector<ElementLayout, 8> Elements;

public:
  /// Create a structure layout.
  ///
  /// \param strategy - how much leeway the algorithm has to rearrange
  ///   and combine the storage of fields
  /// \param kind - the kind of layout to perform, including whether the
  ///   layout must include the reference-counting header
  /// \param typeToFill - if present, must be an opaque type whose body
  ///   will be filled with this layout
  StructLayout(IRGenModule &IGM, NominalTypeDecl *decl,
               LayoutKind kind, LayoutStrategy strategy,
               ArrayRef<const TypeInfo *> fields,
               llvm::StructType *typeToFill = 0);

  /// Create a structure layout from a builder.
  StructLayout(const StructLayoutBuilder &builder,
               NominalTypeDecl *decl,
               llvm::Type *type,
               ArrayRef<ElementLayout> elements)
    : MinimumAlign(builder.getAlignment()),
      MinimumSize(builder.getSize()),
      headerSize(builder.getHeaderSize()),
      SpareBits(builder.getSpareBits()),
      IsFixedLayout(builder.isFixedLayout()),
      IsKnownTriviallyDestroyable(builder.isTriviallyDestroyable()),
      IsKnownBitwiseTakable(builder.isBitwiseTakable()),
      IsKnownCopyable(builder.isCopyable()),
      IsKnownAlwaysFixedSize(builder.isAlwaysFixedSize()),
      Ty(type),
      Elements(elements.begin(), elements.end()) {}

  /// Return the element layouts.  This is parallel to the fields
  /// passed in the constructor.
  ArrayRef<ElementLayout> getElements() const { return Elements; }
  const ElementLayout &getElement(unsigned i) const { return Elements[i]; }
  
  llvm::Type *getType() const { return Ty; }
  Size getSize() const { return MinimumSize; }
  Size getHeaderSize() const { return headerSize; }
  Alignment getAlignment() const { return MinimumAlign; }
  const SpareBitVector &getSpareBits() const { return SpareBits; }
  SpareBitVector &getSpareBits() { return SpareBits; }
  bool isKnownEmpty() const { return isFixedLayout() && MinimumSize.isZero(); }
  IsTriviallyDestroyable_t isTriviallyDestroyable() const {
    return IsKnownTriviallyDestroyable;
  }
  IsBitwiseTakable_t isBitwiseTakable() const {
    return IsKnownBitwiseTakable;
  }
  IsCopyable_t isCopyable() const {
    return IsKnownCopyable;
  }
  IsFixedSize_t isAlwaysFixedSize() const {
    return IsKnownAlwaysFixedSize;
  }

  bool isFixedLayout() const { return IsFixedLayout; }
  llvm::Constant *emitSize(IRGenModule &IGM) const;
  llvm::Constant *emitAlignMask(IRGenModule &IGM) const;

  /// Bitcast the given pointer to this type.
  Address emitCastTo(IRGenFunction &IGF, llvm::Value *ptr,
                     const llvm::Twine &name = "") const;
};

Size getDefaultActorStorageFieldOffset(IRGenModule &IGM);
Size getNonDefaultDistributedActorStorageFieldOffset(IRGenModule &IGM);

} // end namespace irgen
} // end namespace swift

#endif

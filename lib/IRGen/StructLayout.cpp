//===--- StructLayout.cpp - Layout of structures --------------------------===//
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
//  This file implements algorithms for laying out structures.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/ErrorHandling.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsIRGen.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/Basic/Assertions.h"

#include "BitPatternBuilder.h"
#include "Field.h"
#include "FixedTypeInfo.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "StructLayout.h"
#include "TypeInfo.h"

using namespace swift;
using namespace irgen;

/// Does this layout kind require a heap header?
static bool requiresHeapHeader(LayoutKind kind) {
  switch (kind) {
  case LayoutKind::NonHeapObject: return false;
  case LayoutKind::HeapObject: return true;
  }
  llvm_unreachable("bad layout kind!");
}

/// Perform structure layout on the given types.
StructLayout::StructLayout(IRGenModule &IGM, std::optional<CanType> type,
                           LayoutKind layoutKind, LayoutStrategy strategy,
                           ArrayRef<const TypeInfo *> types,
                           llvm::StructType *typeToFill) {
  NominalTypeDecl *decl = nullptr;

  if (type) {
    decl = type->getAnyNominal();
  }

  Elements.reserve(types.size());

  // Fill in the Elements array.
  for (auto type : types)
    Elements.push_back(ElementLayout::getIncomplete(*type));

  assert(typeToFill == nullptr || typeToFill->isOpaque());

  StructLayoutBuilder builder(IGM);

  // Add the heap header if necessary.
  if (requiresHeapHeader(layoutKind)) {
    builder.addHeapHeader();
  }

  auto triviallyDestroyable = (decl && decl->getValueTypeDestructor())
    ? IsNotTriviallyDestroyable : IsTriviallyDestroyable;
  auto copyable = (decl && !decl->canBeCopyable())
    ? IsNotCopyable : IsCopyable;
  IsBitwiseTakable_t bitwiseTakable = IsBitwiseTakable;

  if (decl && decl->getAttrs().hasAttribute<SensitiveAttr>()) {
    triviallyDestroyable = IsNotTriviallyDestroyable;
    bitwiseTakable = IsNotBitwiseTakable;
  }

  // Handle a raw layout specification on a struct.
  RawLayoutAttr *rawLayout = nullptr;
  if (decl) {
    rawLayout = decl->getAttrs().getAttribute<RawLayoutAttr>();
  }
  if (rawLayout && type) {
    auto sd = cast<StructDecl>(decl);
    IsKnownTriviallyDestroyable = triviallyDestroyable;
    IsKnownBitwiseTakable = bitwiseTakable;
    SpareBits.clear();
    assert(!copyable);
    IsKnownCopyable = copyable;
    assert(builder.getHeaderSize() == Size(0));
    headerSize = Size(0);
    IsLoadable = false;

    auto &Diags = IGM.Context.Diags;
    // Fixed size and alignment specified.
    if (auto sizeAndAlign = rawLayout->getSizeAndAlignment()) {
      auto size = Size(sizeAndAlign->first);
      auto requestedAlignment = Alignment(sizeAndAlign->second);
      MinimumAlign = IGM.getCappedAlignment(requestedAlignment);
      if (requestedAlignment > MinimumAlign) {
        Diags.diagnose(rawLayout->getLocation(),
                       diag::alignment_more_than_maximum,
                       MinimumAlign.getValue());
      }
      
      MinimumSize = size;
      SpareBits.extendWithClearBits(MinimumSize.getValueInBits());
      IsFixedLayout = true;
      IsKnownAlwaysFixedSize = IsFixedSize;
    } else if (auto likeType = rawLayout->getResolvedScalarLikeType(sd)) {
      // If our likeType is dependent, then all calls to try and lay it out will
      // be non-fixed, but in a concrete case we want a fixed layout, so try to
      // substitute it out.
      auto subs = (*type)->getContextSubstitutionMap(IGM.getSwiftModule(), decl);
      auto loweredLikeType = IGM.getLoweredType(likeType->subst(subs));
      const TypeInfo &likeTypeInfo = IGM.getTypeInfo(loweredLikeType);
                                      
      // Take layout attributes from the like type.
      if (const FixedTypeInfo *likeFixedType = dyn_cast<FixedTypeInfo>(&likeTypeInfo)) {
        MinimumSize = likeFixedType->getFixedSize();
        SpareBits.extendWithClearBits(MinimumSize.getValueInBits());
        MinimumAlign = likeFixedType->getFixedAlignment();
        IsFixedLayout = true;
        IsKnownAlwaysFixedSize = IsFixedSize;

        // @_rawLayout(like: T) has an optional `movesAsLike` which enforces that
        // a value of this raw layout type should have the same move semantics
        // as the like its like.
        if (rawLayout->shouldMoveAsLikeType()) {
          IsKnownTriviallyDestroyable = likeFixedType->isTriviallyDestroyable(ResilienceExpansion::Maximal);
          IsKnownBitwiseTakable = likeFixedType->isBitwiseTakable(ResilienceExpansion::Maximal);
        }
      } else {
        MinimumSize = Size(0);
        MinimumAlign = Alignment(1);
        IsFixedLayout = false;
        IsKnownAlwaysFixedSize = IsNotFixedSize;

        // We don't know our like type, so assume we're not known to be bitwise
        // takable.
        if (rawLayout->shouldMoveAsLikeType()) {
          IsKnownTriviallyDestroyable = IsNotTriviallyDestroyable;
          IsKnownBitwiseTakable = IsNotBitwiseTakable;
        }
      }
    } else if (auto likeArray = rawLayout->getResolvedArrayLikeTypeAndCount(sd)) {
      auto elementType = likeArray->first;
      unsigned count = likeArray->second;
      
      auto subs = (*type)->getContextSubstitutionMap(IGM.getSwiftModule(), decl);
      auto loweredElementType = IGM.getLoweredType(elementType.subst(subs));
      const TypeInfo &likeTypeInfo = IGM.getTypeInfo(loweredElementType);
      
      // Take layout attributes from the like type.
      if (const FixedTypeInfo *likeFixedType = dyn_cast<FixedTypeInfo>(&likeTypeInfo)) {
        MinimumSize = likeFixedType->getFixedStride() * count;
        SpareBits.extendWithClearBits(MinimumSize.getValueInBits());
        MinimumAlign = likeFixedType->getFixedAlignment();
        IsFixedLayout = true;
        IsKnownAlwaysFixedSize = IsFixedSize;
      } else {
        MinimumSize = Size(0);
        MinimumAlign = Alignment(1);
        IsFixedLayout = false;
        IsKnownAlwaysFixedSize = IsNotFixedSize;
      }
    } else {
      llvm_unreachable("unhandled raw layout variant?");
    }
    
    // Set the LLVM struct type for a fixed layout according to the stride and
    // alignment we determined.
    if (IsKnownAlwaysFixedSize) {
      auto eltTy = llvm::IntegerType::get(IGM.getLLVMContext(), 8);
      auto bodyTy = llvm::ArrayType::get(eltTy, MinimumSize.getValue());
      if (typeToFill) {
        typeToFill->setBody(bodyTy, /*packed*/ true);
        Ty = typeToFill;
      } else {
        Ty = llvm::StructType::get(IGM.getLLVMContext(), bodyTy, /*packed*/ true);
      }
    } else {
      Ty = (typeToFill ? typeToFill : IGM.OpaqueTy);
    }
  } else {
    bool nonEmpty = builder.addFields(Elements, strategy);

    // Special-case: there's nothing to store.
    // In this case, produce an opaque type;  this tends to cause lovely
    // assertions.
    if (!nonEmpty) {
      assert(!builder.empty() == requiresHeapHeader(layoutKind));
      MinimumAlign = Alignment(1);
      MinimumSize = Size(0);
      headerSize = builder.getHeaderSize();
      SpareBits.clear();
      IsFixedLayout = true;
      IsLoadable = true;
      IsKnownTriviallyDestroyable = triviallyDestroyable & builder.isTriviallyDestroyable();
      IsKnownBitwiseTakable = builder.isBitwiseTakable();
      IsKnownAlwaysFixedSize = builder.isAlwaysFixedSize();
      IsKnownCopyable = copyable & builder.isCopyable();
      Ty = (typeToFill ? typeToFill : IGM.OpaqueTy);
    } else {
      MinimumAlign = builder.getAlignment();
      MinimumSize = builder.getSize();
      headerSize = builder.getHeaderSize();
      SpareBits = builder.getSpareBits();
      IsFixedLayout = builder.isFixedLayout();
      IsLoadable = builder.isLoadable();
      IsKnownTriviallyDestroyable = triviallyDestroyable & builder.isTriviallyDestroyable();
      IsKnownBitwiseTakable = bitwiseTakable & builder.isBitwiseTakable();
      IsKnownAlwaysFixedSize = builder.isAlwaysFixedSize();
      IsKnownCopyable = copyable & builder.isCopyable();
      if (typeToFill) {
        builder.setAsBodyOfStruct(typeToFill);
        Ty = typeToFill;
      } else {
        Ty = builder.getAsAnonStruct();
      }
    }
  }

  assert(typeToFill == nullptr || Ty == typeToFill);

  // If the struct is not @frozen, it will have a dynamic
  // layout outside of its resilience domain.
  if (decl) {
    if (IGM.isResilient(decl, ResilienceExpansion::Minimal))
      IsKnownAlwaysFixedSize = IsNotFixedSize;

    applyLayoutAttributes(IGM, decl, IsFixedLayout, MinimumAlign);
  }
}

void irgen::applyLayoutAttributes(IRGenModule &IGM,
                                  NominalTypeDecl *decl,
                                  bool IsFixedLayout,
                                  Alignment &MinimumAlign) {
  auto &Diags = IGM.Context.Diags;

  if (auto alignment = decl->getAttrs().getAttribute<AlignmentAttr>()) {
    assert(!decl->getAttrs().hasAttribute<RawLayoutAttr>()
           && "_alignment and _rawLayout not supported together");
    auto value = alignment->getValue();
    assert(value != 0 && ((value - 1) & value) == 0
           && "alignment not a power of two!");
    
    if (!IsFixedLayout)
      Diags.diagnose(alignment->getLocation(),
                     diag::alignment_dynamic_type_layout_unsupported);
    else if (value < MinimumAlign.getValue())
      Diags.diagnose(alignment->getLocation(),
                   diag::alignment_less_than_natural, MinimumAlign.getValue());
    else {
      auto requestedAlignment = Alignment(value);
      MinimumAlign = IGM.getCappedAlignment(requestedAlignment);
      if (requestedAlignment > MinimumAlign)
        Diags.diagnose(alignment->getLocation(),
                       diag::alignment_more_than_maximum,
                       MinimumAlign.getValue());
    }
  }
}

llvm::Constant *StructLayout::emitSize(IRGenModule &IGM) const {
  assert(isFixedLayout());
  return IGM.getSize(getSize());
}

llvm::Constant *StructLayout::emitAlignMask(IRGenModule &IGM) const {
  assert(isFixedLayout());
  return IGM.getSize(getAlignment().asSize() - Size(1));
}

/// Bitcast an arbitrary pointer to be a pointer to this type.
Address StructLayout::emitCastTo(IRGenFunction &IGF,
                                 llvm::Value *ptr,
                                 const llvm::Twine &name) const {
  llvm::Value *addr =
    IGF.Builder.CreateBitCast(ptr, getType()->getPointerTo(), name);
  return Address(addr, getType(), getAlignment());
}

Address ElementLayout::project(IRGenFunction &IGF, Address baseAddr,
                               NonFixedOffsets offsets,
                               const llvm::Twine &suffix) const {
  switch (getKind()) {
  case Kind::Empty:
  case Kind::EmptyTailAllocatedCType:
    return getType().getUndefAddress();

  case Kind::Fixed:
    return IGF.Builder.CreateStructGEP(baseAddr,
                                       getStructIndex(),
                                       getByteOffset(),
                                 baseAddr.getAddress()->getName() + suffix);

  case Kind::NonFixed: {
    assert(offsets.has_value());
    llvm::Value *offset =
      offsets.value()->getOffsetForIndex(IGF, getNonFixedElementIndex());
    return IGF.emitByteOffsetGEP(baseAddr.getAddress(), offset, getType(),
                                 baseAddr.getAddress()->getName() + suffix);
  }

  case Kind::InitialNonFixedSize:
    return IGF.Builder.CreateElementBitCast(
        baseAddr, getType().getStorageType(),
        baseAddr.getAddress()->getName() + suffix);
  }
  llvm_unreachable("bad element layout kind");
}

void StructLayoutBuilder::addHeapHeader() {
  assert(StructFields.empty() && "adding heap header at a non-zero offset");
  CurSize = IGM.RefCountedStructSize;
  CurAlignment = IGM.getPointerAlignment();
  StructFields.push_back(IGM.RefCountedStructTy);
  headerSize = CurSize;
}

void StructLayoutBuilder::addNSObjectHeader() {
  assert(StructFields.empty() && "adding heap header at a non-zero offset");
  CurSize = IGM.getPointerSize();
  CurAlignment = IGM.getPointerAlignment();
  StructFields.push_back(IGM.ObjCClassPtrTy);
  headerSize = CurSize;
}

void StructLayoutBuilder::addDefaultActorHeader(ElementLayout &elt) {
  assert(StructFields.size() == 1 &&
         StructFields[0] == IGM.RefCountedStructTy &&
         "adding default actor header at wrong offset");

  // These must match the DefaultActor class in Actor.h.
  auto size = NumWords_DefaultActor * IGM.getPointerSize();
  auto align = Alignment(Alignment_DefaultActor);
  auto ty = llvm::ArrayType::get(IGM.Int8PtrTy, NumWords_DefaultActor);

  // Note that we align the *entire structure* to the new alignment,
  // not the storage we're adding.  Otherwise we would potentially
  // get internal padding.
  assert(CurSize.isMultipleOf(IGM.getPointerSize()));
  assert(align >= CurAlignment);
  assert(CurSize == getDefaultActorStorageFieldOffset(IGM));
  elt.completeFixed(IsNotTriviallyDestroyable, CurSize, /*struct index*/ 1);
  CurSize += size;
  CurAlignment = align;
  StructFields.push_back(ty);
  headerSize = CurSize;
}

void StructLayoutBuilder::addNonDefaultDistributedActorHeader(ElementLayout &elt) {
  assert(StructFields.size() == 1 &&
         StructFields[0] == IGM.RefCountedStructTy &&
         "adding default actor header at wrong offset");

  // These must match the NonDefaultDistributedActor class in Actor.h.
  auto size = NumWords_NonDefaultDistributedActor * IGM.getPointerSize();
  auto align = Alignment(Alignment_NonDefaultDistributedActor);
  auto ty = llvm::ArrayType::get(IGM.Int8PtrTy, NumWords_NonDefaultDistributedActor);

  // Note that we align the *entire structure* to the new alignment,
  // not the storage we're adding.  Otherwise we would potentially
  // get internal padding.
  assert(CurSize.isMultipleOf(IGM.getPointerSize()));
  assert(align >= CurAlignment);
  assert(CurSize == getNonDefaultDistributedActorStorageFieldOffset(IGM));
  elt.completeFixed(IsNotTriviallyDestroyable, CurSize, /*struct index*/ 1);
  CurSize += size;
  CurAlignment = align;
  StructFields.push_back(ty);
  headerSize = CurSize;
}

Size irgen::getDefaultActorStorageFieldOffset(IRGenModule &IGM) {
  return IGM.RefCountedStructSize;
}

Size irgen::getNonDefaultDistributedActorStorageFieldOffset(IRGenModule &IGM) {
  return IGM.RefCountedStructSize;
}

bool StructLayoutBuilder::addFields(llvm::MutableArrayRef<ElementLayout> elts,
                                    LayoutStrategy strategy) {
  // Track whether we've added any storage to our layout.
  bool addedStorage = false;

  // Loop through the elements.  The only valid field in each element
  // is Type; StructIndex and ByteOffset need to be laid out.
  for (auto &elt : elts) {
    addedStorage |= addField(elt, strategy);
  }

  return addedStorage;
}

bool StructLayoutBuilder::addField(ElementLayout &elt,
                                  LayoutStrategy strategy) {
  auto &eltTI = elt.getType();
  IsKnownTriviallyDestroyable &= eltTI.isTriviallyDestroyable(ResilienceExpansion::Maximal);
  IsKnownBitwiseTakable &= eltTI.isBitwiseTakable(ResilienceExpansion::Maximal);
  IsKnownAlwaysFixedSize &= eltTI.isFixedSize(ResilienceExpansion::Minimal);
  IsLoadable &= eltTI.isLoadable();
  IsKnownCopyable &= eltTI.isCopyable(ResilienceExpansion::Maximal);

  if (eltTI.isKnownEmpty(ResilienceExpansion::Maximal)) {
    addEmptyElement(elt);
    // If the element type is empty, it adds nothing.
    ++NextNonFixedOffsetIndex;
    return false;
  }
  // TODO: consider using different layout rules.
  // If the rules are changed so that fields aren't necessarily laid
  // out sequentially, the computation of InstanceStart in the
  // RO-data will need to be fixed.

  // If this element is resiliently- or dependently-sized, record
  // that and configure the ElementLayout appropriately.
  if (isa<FixedTypeInfo>(eltTI)) {
    addFixedSizeElement(elt);
  } else {
    addNonFixedSizeElement(elt);
  }
  ++NextNonFixedOffsetIndex;
  return true;
}

void StructLayoutBuilder::addFixedSizeElement(ElementLayout &elt) {
  auto &eltTI = cast<FixedTypeInfo>(elt.getType());

  // Note that, even in the presence of elements with non-fixed
  // size, we continue to compute the minimum size and alignment
  // requirements of the overall aggregate as if all the
  // non-fixed-size elements were empty.  This gives us minimum
  // bounds on the size and alignment of the aggregate.

  // The struct alignment is the max of the alignment of the fields.
  CurAlignment = std::max(CurAlignment, eltTI.getFixedAlignment());

  // If the current tuple size isn't a multiple of the field's
  // required alignment, we need to pad out.
  Alignment eltAlignment = eltTI.getFixedAlignment();
  if (Size offsetFromAlignment = CurSize % eltAlignment) {
    unsigned paddingRequired
      = eltAlignment.getValue() - offsetFromAlignment.getValue();
    assert(paddingRequired != 0);

    // Regardless, the storage size goes up.
    CurSize += Size(paddingRequired);

    // Add the padding to the fixed layout.
    if (isFixedLayout()) {
      auto paddingTy = llvm::ArrayType::get(IGM.Int8Ty, paddingRequired);
      StructFields.push_back(paddingTy);

      // The padding can be used as spare bits by enum layout.
      auto numBits = Size(paddingRequired).getValueInBits();
      auto mask = llvm::APInt::getAllOnes(numBits);
      CurSpareBits.push_back(SpareBitVector::fromAPInt(mask));
    }
  }

  // If the overall structure so far has a fixed layout, then add
  // this as a field to the layout.
  if (isFixedLayout()) {
    addElementAtFixedOffset(elt);
  // Otherwise, just remember the next non-fixed offset index.
  } else {
    addElementAtNonFixedOffset(elt);
  }
  CurSize += eltTI.getFixedSize();
}

void StructLayoutBuilder::addNonFixedSizeElement(ElementLayout &elt) {
  // If the element is the first non-empty element to be added to the
  // structure, we can assign it a fixed offset (namely zero) despite
  // it not having a fixed size/alignment.
  if (isFixedLayout() && CurSize.isZero()) {
    addNonFixedSizeElementAtOffsetZero(elt);
    IsFixedLayout = false;
    return;
  }

  // Otherwise, we cannot give it a fixed offset, even if all the
  // previous elements are non-fixed.  The problem is not that it has
  // an unknown *size*; it's that it has an unknown *alignment*, which
  // might force us to introduce padding.  Absent some sort of user
  // "max alignment" annotation (or having reached the platform
  // maximum alignment, if there is one), these are part and parcel.
  IsFixedLayout = false;
  addElementAtNonFixedOffset(elt);

  assert(!IsKnownAlwaysFixedSize);
}

/// Add an empty element to the aggregate.
void StructLayoutBuilder::addEmptyElement(ElementLayout &elt) {
  auto byteOffset = isFixedLayout() ? CurSize : Size(0);
  elt.completeEmpty(elt.getType().isTriviallyDestroyable(ResilienceExpansion::Maximal), byteOffset);
}

/// Add an element at the fixed offset of the current end of the
/// aggregate.
void StructLayoutBuilder::addElementAtFixedOffset(ElementLayout &elt) {
  assert(isFixedLayout());
  auto &eltTI = cast<FixedTypeInfo>(elt.getType());

  elt.completeFixed(elt.getType().isTriviallyDestroyable(ResilienceExpansion::Maximal),
                    CurSize, StructFields.size());
  StructFields.push_back(elt.getType().getStorageType());
  
  // Carry over the spare bits from the element.
  CurSpareBits.push_back(eltTI.getSpareBits());
}

/// Add an element at a non-fixed offset to the aggregate.
void StructLayoutBuilder::addElementAtNonFixedOffset(ElementLayout &elt) {
  assert(!isFixedLayout());
  elt.completeNonFixed(elt.getType().isTriviallyDestroyable(ResilienceExpansion::Maximal),
                       NextNonFixedOffsetIndex);
  CurSpareBits = SmallVector<SpareBitVector, 8>(); // clear spare bits
}

/// Add a non-fixed-size element to the aggregate at offset zero.
void StructLayoutBuilder::addNonFixedSizeElementAtOffsetZero(ElementLayout &elt) {
  assert(isFixedLayout());
  assert(!isa<FixedTypeInfo>(elt.getType()));
  assert(CurSize.isZero());
  elt.completeInitialNonFixedSize(elt.getType().isTriviallyDestroyable(ResilienceExpansion::Maximal));
  CurSpareBits = SmallVector<SpareBitVector, 8>(); // clear spare bits
}

/// Produce the current fields as an anonymous structure.
llvm::StructType *StructLayoutBuilder::getAsAnonStruct() const {
  auto ty = llvm::StructType::get(IGM.getLLVMContext(), StructFields,
                                  /*isPacked*/ true);
  assert((!isFixedLayout()
          || IGM.DataLayout.getStructLayout(ty)->getSizeInBytes()
            == CurSize.getValue())
         && "LLVM size of fixed struct type does not match StructLayout size");
  return ty;
}

/// Set the current fields as the body of the given struct type.
void StructLayoutBuilder::setAsBodyOfStruct(llvm::StructType *type) const {
  assert(type->isOpaque());
  type->setBody(StructFields, /*isPacked*/ true);
  assert((!isFixedLayout()
          || IGM.DataLayout.getStructLayout(type)->getSizeInBytes()
            == CurSize.getValue())
         && "LLVM size of fixed struct type does not match StructLayout size");
}

/// Return the spare bit mask of the structure built so far.
SpareBitVector StructLayoutBuilder::getSpareBits() const {
  auto spareBits = BitPatternBuilder(IGM.Triple.isLittleEndian());
  for (const auto &v : CurSpareBits) {
    spareBits.append(v);
  }
  return spareBits.build();
}

unsigned irgen::getNumFields(const NominalTypeDecl *target) {
  auto numFields =
    target->getStoredPropertiesAndMissingMemberPlaceholders().size();
  if (auto cls = dyn_cast<ClassDecl>(target)) {
    if (cls->isRootDefaultActor()) {
      numFields++;
    } else if (cls->isNonDefaultExplicitDistributedActor()) {
      numFields++;
    }
  }
  return numFields;
}

void irgen::forEachField(IRGenModule &IGM, const NominalTypeDecl *typeDecl,
                         llvm::function_ref<void(Field field)> fn) {
  auto classDecl = dyn_cast<ClassDecl>(typeDecl);
  if (classDecl) {
    if (classDecl->isRootDefaultActor()) {
      fn(Field::DefaultActorStorage);
    } else if (classDecl->isNonDefaultExplicitDistributedActor()) {
      fn(Field::NonDefaultDistributedActorStorage);
    }
  }

  for (auto decl :
         typeDecl->getStoredPropertiesAndMissingMemberPlaceholders()) {
    if (auto var = dyn_cast<VarDecl>(decl)) {
      fn(var);
    } else {
      fn(cast<MissingMemberDecl>(decl));
    }
  }
}

SILType Field::getType(IRGenModule &IGM, SILType baseType) const {
  switch (getKind()) {
  case Field::Var:
    return baseType.getFieldType(getVarDecl(), IGM.getSILModule(),
                                 TypeExpansionContext::minimal());
  case Field::MissingMember:
    llvm_unreachable("cannot ask for type of missing member");
  case Field::DefaultActorStorage:
    return SILType::getPrimitiveObjectType(
                             IGM.Context.TheDefaultActorStorageType);
  case Field::NonDefaultDistributedActorStorage:
    return SILType::getPrimitiveObjectType(
                             IGM.Context.TheNonDefaultDistributedActorStorageType);
  }
  llvm_unreachable("bad field kind");
}

Type Field::getInterfaceType(IRGenModule &IGM) const {
  switch (getKind()) {
  case Field::Var:
    return getVarDecl()->getInterfaceType();
  case Field::MissingMember:
    llvm_unreachable("cannot ask for type of missing member");
  case Field::DefaultActorStorage:
    return IGM.Context.TheDefaultActorStorageType;
  case Field::NonDefaultDistributedActorStorage:
    return IGM.Context.TheNonDefaultDistributedActorStorageType;
  }
  llvm_unreachable("bad field kind");
}

StringRef Field::getName() const {
  switch (getKind()) {
  case Field::Var:
    return getVarDecl()->getName().str();
  case Field::MissingMember:
    llvm_unreachable("cannot ask for type of missing member");
  case Field::DefaultActorStorage:
    return DEFAULT_ACTOR_STORAGE_FIELD_NAME;
  case Field::NonDefaultDistributedActorStorage:
    return NON_DEFAULT_DISTRIBUTED_ACTOR_STORAGE_FIELD_NAME;
  }
  llvm_unreachable("bad field kind");
}

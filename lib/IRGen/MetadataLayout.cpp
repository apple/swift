//===--- MetadataLayout.cpp - Metadata construct layout -------------------===//
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
//  This file includes code for laying out type metadata.
// 
//  It also implements certain low-level access routines for type metadata.
//  These routines are generally declared in one of two different places:
//
//    - Mid-level routines to extract data from metadata are declared in
//      GenMeta.h.  This file is a sort of sub-module of GenMeta.cpp.
//
//    - Low-level routines to project the addresses of fields in metadata
//      are declared in MetadataLayout.h.
//
//===----------------------------------------------------------------------===//

#include "MetadataLayout.h"
#include "GenMeta.h"

#include "ClassMetadataVisitor.h"
#include "EnumMetadataVisitor.h"
#include "IRGenFunction.h"
#include "StructMetadataVisitor.h"
#include "ForeignClassMetadataVisitor.h"
#include "TupleMetadataVisitor.h"

#include "swift/Basic/LLVM.h"
#include "swift/SIL/SILModule.h"
#include "llvm/ADT/Optional.h"

using namespace swift;
using namespace irgen;

namespace {

template <class Impl, template <class> class Base>
class LayoutScanner : public Base<Impl> {
  Optional<Size> AddressPoint;

protected:
  Optional<Size> DynamicOffsetBase;

  template <class... As>
  LayoutScanner(As &&... args) : Base<Impl>(std::forward<As>(args)...) {}

public:
  using StoredOffset = MetadataLayout::StoredOffset;

  void noteAddressPoint() { AddressPoint = this->NextOffset; }
  StoredOffset getNextOffset() const {
    if (DynamicOffsetBase) {
      return StoredOffset(this->NextOffset - *DynamicOffsetBase,
                          StoredOffset::Dynamic);
    }

    return StoredOffset(this->NextOffset - *AddressPoint,
                        StoredOffset::Static);
  }

  Size getAddressPoint() const {
    return *AddressPoint;
  }

  MetadataSize getMetadataSize() const {
    assert(AddressPoint.has_value() && !AddressPoint->isInvalid()
           && "did not find address point?!");
    assert(*AddressPoint < this->NextOffset
           && "address point is after end?!");
    return {this->NextOffset, *AddressPoint};
  }
};

}

ClassMetadataLayout &IRGenModule::getClassMetadataLayout(ClassDecl *decl) {
  assert(!decl->isForeign() && "Use getForeignMetadataLayout()");
  return cast<ClassMetadataLayout>(
                        getMetadataLayout(static_cast<NominalTypeDecl*>(decl)));
}

ForeignClassMetadataLayout &IRGenModule::getForeignMetadataLayout(
                                                          ClassDecl *decl) {
  assert(decl->isForeign() && "Use getMetadataLayout()");
  return cast<ForeignClassMetadataLayout>(
           getMetadataLayout(static_cast<NominalTypeDecl*>(decl)));
}

EnumMetadataLayout &IRGenModule::getMetadataLayout(EnumDecl *decl) {
  return cast<EnumMetadataLayout>(
                        getMetadataLayout(static_cast<NominalTypeDecl*>(decl)));
}

StructMetadataLayout &IRGenModule::getMetadataLayout(StructDecl *decl) {
  return cast<StructMetadataLayout>(
                        getMetadataLayout(static_cast<NominalTypeDecl*>(decl)));
}

NominalMetadataLayout &IRGenModule::getNominalMetadataLayout(
                                                    NominalTypeDecl *decl) {
  return cast<NominalMetadataLayout>(getMetadataLayout(decl));
}

MetadataLayout &IRGenModule::getMetadataLayout(NominalTypeDecl *decl) {
  auto &entry = MetadataLayouts[decl];
  if (!entry) {
    if (auto theClass = dyn_cast<ClassDecl>(decl)) {
      if (theClass->getForeignClassKind() == ClassDecl::ForeignKind::CFType ||
          theClass->isForeignReferenceType())
        entry = new ForeignClassMetadataLayout(*this, theClass);
      else
        entry = new ClassMetadataLayout(*this, theClass);
    } else if (auto theEnum = dyn_cast<EnumDecl>(decl)) {
      entry = new EnumMetadataLayout(*this, theEnum);
    } else if (auto theStruct = dyn_cast<StructDecl>(decl)) {
      entry = new StructMetadataLayout(*this, theStruct);
    } else {
      llvm_unreachable("bad nominal type!");
    }
  }
  return *cast<MetadataLayout>(entry);
}

void IRGenModule::destroyMetadataLayoutMap() {
  for (auto &entry : MetadataLayouts) {
    entry.second->destroy();
  }
}

void MetadataLayout::destroy() const {
  switch (getKind()) {
  case Kind::Class:
    delete cast<ClassMetadataLayout>(this);
    return;

  case Kind::Struct:
    delete cast<StructMetadataLayout>(this);
    return;

  case Kind::Enum:
    delete cast<EnumMetadataLayout>(this);
    return;

  case Kind::ForeignClass:
    delete cast<ForeignClassMetadataLayout>(this);
    return;
  }
  llvm_unreachable("bad kind");
}

/******************************* NOMINAL TYPES ********************************/

Offset NominalMetadataLayout::emitOffset(IRGenFunction &IGF,
                                         StoredOffset offset) const {
  assert(offset.isValid());

  if (offset.isStatic())
    return Offset(offset.getStaticOffset());

  Address layoutAddr(IGF.IGM.getAddrOfClassMetadataBounds(
                         cast<ClassDecl>(getDecl()), NotForDefinition),
                     IGF.IGM.ClassMetadataBaseOffsetTy,
                     IGF.IGM.getPointerAlignment());

  auto offsetBaseAddr = IGF.Builder.CreateStructGEP(layoutAddr, 0, Size(0));

  // FIXME: Should this be an invariant load?
  llvm::Value *offsetVal = IGF.Builder.CreateLoad(offsetBaseAddr, "base");

  auto relativeOffset = offset.getRelativeOffset().getValue();
  if (relativeOffset != 0) {
    offsetVal = IGF.Builder.CreateAdd(offsetVal,
                                      llvm::ConstantInt::get(IGF.IGM.SizeTy,
                                                             relativeOffset));
  }

  return Offset(offsetVal);
}

Size
NominalMetadataLayout::getStaticGenericRequirementsOffset() const {
  return GenericRequirements.getStaticOffset();
}

Offset
NominalMetadataLayout::getGenericRequirementsOffset(IRGenFunction &IGF) const {
  return emitOffset(IGF, GenericRequirements);
}

static llvm::Value *emitLoadOfGenericRequirement(IRGenFunction &IGF,
                                                 llvm::Value *metadata,
                                                 NominalTypeDecl *decl,
                                                 unsigned reqtIndex,
                                                 llvm::Type *reqtTy) {
  auto offset =
    IGF.IGM.getNominalMetadataLayout(decl).getGenericRequirementsOffset(IGF);
  offset = offset.offsetBy(IGF, Size(reqtIndex * IGF.IGM.getPointerSize()));

  auto slot = IGF.emitAddressAtOffset(metadata, offset, reqtTy,
                                      IGF.IGM.getPointerAlignment());
  auto witness = IGF.emitInvariantLoad(slot);
  return witness;
}

/// Given a reference to nominal type metadata of the given type,
/// derive a reference to the nth argument metadata.  The type must
/// have generic arguments.
llvm::Value *irgen::emitArgumentMetadataRef(IRGenFunction &IGF,
                                            NominalTypeDecl *decl,
                                      const GenericTypeRequirements &reqts,
                                            unsigned reqtIndex,
                                            llvm::Value *metadata) {
  assert(reqts.getRequirements()[reqtIndex].getKind()
           == GenericRequirement::Kind::Metadata);
  return emitLoadOfGenericRequirement(IGF, metadata, decl, reqtIndex,
                                      IGF.IGM.TypeMetadataPtrTy);
}

/// Given a reference to nominal type metadata of the given type,
/// derive a reference to the nth argument metadata pack.  The type must
/// have generic arguments.
llvm::Value *irgen::emitArgumentMetadataPackRef(IRGenFunction &IGF,
                                                NominalTypeDecl *decl,
                                      const GenericTypeRequirements &reqts,
                                                unsigned reqtIndex,
                                                llvm::Value *metadata) {
  assert(reqts.getRequirements()[reqtIndex].isMetadataPack());
  return emitLoadOfGenericRequirement(IGF, metadata, decl, reqtIndex,
                                      IGF.IGM.TypeMetadataPtrPtrTy);
}

/// Given a reference to nominal type metadata of the given type,
/// derive a reference to a protocol witness table for the nth
/// argument metadata.  The type must have generic arguments.
llvm::Value *irgen::emitArgumentWitnessTableRef(IRGenFunction &IGF,
                                                NominalTypeDecl *decl,
                                          const GenericTypeRequirements &reqts,
                                                unsigned reqtIndex,
                                                llvm::Value *metadata) {
  assert(reqts.getRequirements()[reqtIndex].getKind()
           == GenericRequirement::Kind::WitnessTable);
  return emitLoadOfGenericRequirement(IGF, metadata, decl, reqtIndex,
                                      IGF.IGM.WitnessTablePtrTy);
}

/// Given a reference to nominal type metadata of the given type,
/// derive a reference to a protocol witness table pack for the nth
/// argument metadata.  The type must have generic arguments.
llvm::Value *irgen::emitArgumentWitnessTablePackRef(IRGenFunction &IGF,
                                                    NominalTypeDecl *decl,
                                          const GenericTypeRequirements &reqts,
                                                    unsigned reqtIndex,
                                                    llvm::Value *metadata) {
  assert(reqts.getRequirements()[reqtIndex].isWitnessTablePack());
  return emitLoadOfGenericRequirement(IGF, metadata, decl, reqtIndex,
                                      IGF.IGM.WitnessTablePtrPtrTy);
}

/// Given a reference to nominal type metadata of the given type,
/// derive a reference to the pack shape for the nth argument
/// metadata.  The type must have generic arguments.
llvm::Value *irgen::emitArgumentPackShapeRef(IRGenFunction &IGF,
                                             NominalTypeDecl *decl,
                                       const GenericTypeRequirements &reqts,
                                             unsigned reqtIndex,
                                             llvm::Value *metadata) {
  assert(reqts.getRequirements()[reqtIndex].isShape());
  return emitLoadOfGenericRequirement(IGF, metadata, decl, reqtIndex,
                                      IGF.IGM.SizeTy);
}

Address irgen::emitAddressOfFieldOffsetVector(IRGenFunction &IGF,
                                              llvm::Value *metadata,
                                              NominalTypeDecl *decl) {
  auto &layout = IGF.IGM.getMetadataLayout(decl);
  auto offset = [&]() {
    if (isa<ClassDecl>(decl)) {
      return cast<ClassMetadataLayout>(layout)
        .getFieldOffsetVectorOffset(IGF);
    } else {
      assert(isa<StructDecl>(decl));
      return cast<StructMetadataLayout>(layout)
        .getFieldOffsetVectorOffset();
    }
  }();

  auto *elementSize = IGF.IGM.SizeTy;
  if (isa<StructDecl>(decl))
    elementSize = IGF.IGM.Int32Ty;

  return IGF.emitAddressAtOffset(metadata, offset, elementSize,
                                 IGF.IGM.getPointerAlignment());
}

/********************************** CLASSES ***********************************/

ClassMetadataLayout::ClassMetadataLayout(IRGenModule &IGM, ClassDecl *decl)
    : NominalMetadataLayout(Kind::Class, decl), NumImmediateMembers(0) {

  struct Scanner : LayoutScanner<Scanner, ClassMetadataScanner> {
    using super = LayoutScanner;

    ClassMetadataLayout &Layout;

    bool IsInTargetFields = false;

    Scanner(IRGenModule &IGM, ClassDecl *decl, ClassMetadataLayout &layout)
      : super(IGM, decl), Layout(layout) {}

    void noteResilientSuperclass() {
      Layout.HasResilientSuperclass = true;
    }

    void noteStartOfImmediateMembers(ClassDecl *forClass) {
      // If our superclass is resilient to us, or the class itself is resilient
      // to us, we will access metadata members relative to a base offset.
      if (forClass == Target) {
        Layout.StartOfImmediateMembers = getNextOffset();

        if (Layout.HasResilientSuperclass ||
            IGM.hasResilientMetadata(forClass, ResilienceExpansion::Maximal)) {
          assert(!DynamicOffsetBase);
          DynamicOffsetBase = NextOffset;
        }
      }
    }

    void addClassSize() {
      Layout.MetadataSize = getNextOffset();
      super::addClassSize();
    }

    void addClassAddressPoint() {
      Layout.MetadataAddressPoint = getNextOffset();
      super::addClassAddressPoint();
    }

    void addInstanceSize() {
      Layout.InstanceSize = getNextOffset();
      super::addInstanceSize();
    }

    void addInstanceAlignMask() {
      Layout.InstanceAlignMask = getNextOffset();
      super::addInstanceAlignMask();
    }

    void noteStartOfGenericRequirements(ClassDecl *forClass) {
      if (forClass == Target)
        Layout.GenericRequirements = getNextOffset();
      super::noteStartOfGenericRequirements(forClass);
    }

    void addGenericRequirement(GenericRequirement requirement,
                               ClassDecl *forClass) {
      if (forClass == Target) {
        ++Layout.NumImmediateMembers;
      }
      super::addGenericRequirement(requirement, forClass);
    }

    void addReifiedVTableEntry(SILDeclRef fn) {
      if (fn.getDecl()->getDeclContext() == Target) {
        ++Layout.NumImmediateMembers;
        Layout.MethodInfos.try_emplace(fn, getNextOffset());
      }
      super::addReifiedVTableEntry(fn);
    }
    
    void noteNonoverriddenMethod(SILDeclRef fn) {
      if (fn.getDecl()->getDeclContext() == Target) {
        auto impl = VTable->getEntry(IGM.getSILModule(), fn);
        Layout.MethodInfos.try_emplace(fn,
         IGM.getAddrOfSILFunction(impl->getImplementation(), NotForDefinition));
      }
    }

    void noteStartOfFieldOffsets(ClassDecl *forClass) {
      if (forClass == Target) {
        assert(!IsInTargetFields);
        IsInTargetFields = true;
        Layout.FieldOffsetVector = getNextOffset();
      }
      super::noteStartOfFieldOffsets(forClass);
    }

    void noteEndOfFieldOffsets(ClassDecl *forClass) {
      assert(IsInTargetFields == (forClass == Target));
      if (IsInTargetFields)
        IsInTargetFields = false;
      super::noteEndOfFieldOffsets(forClass);
    }

    void addFieldOffset(VarDecl *field) {
      assert(IsInTargetFields ==
              (field->getDeclContext()->getImplementedObjCContext() == Target));
      if (IsInTargetFields) {
        ++Layout.NumImmediateMembers;
        Layout.FieldOffsets.try_emplace(field, getNextOffset());
      }
      super::addFieldOffset(field);
    }

    void addDefaultActorStorageFieldOffset() {
      if (IsInTargetFields) {
        ++Layout.NumImmediateMembers;
      }
      super::addDefaultActorStorageFieldOffset();
    }

    void addNonDefaultDistributedActorStorageFieldOffset() {
      if (IsInTargetFields) {
        ++Layout.NumImmediateMembers;
      }
      super::addNonDefaultDistributedActorStorageFieldOffset();
    }

    void addFieldOffsetPlaceholders(MissingMemberDecl *placeholder) {
      if (placeholder->getDeclContext()->getImplementedObjCContext() == Target) {
        Layout.NumImmediateMembers +=
          placeholder->getNumberOfFieldOffsetVectorEntries();
      }
      super::addFieldOffsetPlaceholders(placeholder);
    }

    void addVTableEntries(ClassDecl *forClass) {
      if (forClass == Target)
        Layout.VTableOffset = getNextOffset();
      super::addVTableEntries(forClass);
    }

    void layout() {
      super::layout();
      Layout.TheSize = getMetadataSize();
    }
  };

  Scanner(IGM, decl, *this).layout();
}

Size ClassMetadataLayout::getMetadataSizeOffset() const {
  assert(MetadataSize.isStatic());
  return MetadataSize.getStaticOffset();
}

Size ClassMetadataLayout::getMetadataAddressPointOffset() const {
  assert(MetadataAddressPoint.isStatic());
  return MetadataAddressPoint.getStaticOffset();
}

Size ClassMetadataLayout::getInstanceSizeOffset() const {
  assert(InstanceSize.isStatic());
  return InstanceSize.getStaticOffset();
}

Size ClassMetadataLayout::getInstanceAlignMaskOffset() const {
  assert(InstanceAlignMask.isStatic());
  return InstanceAlignMask.getStaticOffset();
}

ClassMetadataLayout::MethodInfo
ClassMetadataLayout::getMethodInfo(IRGenFunction &IGF, SILDeclRef method) const{
  auto &stored = getStoredMethodInfo(method);
  switch (stored.TheKind) {
  case MethodInfo::Kind::Offset: {
    auto offset = emitOffset(IGF, stored.TheOffset);
    return MethodInfo(offset);
  }
  case MethodInfo::Kind::DirectImpl:
    return MethodInfo(stored.TheImpl);
  }
  llvm_unreachable("unhandled method info kind!");
}

Offset ClassMetadataLayout::getFieldOffset(IRGenFunction &IGF,
                                           VarDecl *field) const {
  return emitOffset(IGF, getStoredFieldOffset(field));
}

Size ClassMetadataLayout::getStaticFieldOffset(VarDecl *field) const {
  auto &stored = getStoredFieldOffset(field);
  assert(stored.isStatic() && "resilient class metadata layout unsupported!");
  return stored.getStaticOffset();
}

Size
ClassMetadataLayout::getRelativeGenericRequirementsOffset() const {
  return GenericRequirements.getRelativeOffset();
}

Size
ClassMetadataLayout::getStaticFieldOffsetVectorOffset() const {
  return FieldOffsetVector.getStaticOffset();
}

Size
ClassMetadataLayout::getRelativeFieldOffsetVectorOffset() const {
  return FieldOffsetVector.getRelativeOffset();
}

Size
ClassMetadataLayout::getStaticVTableOffset() const {
  return VTableOffset.getStaticOffset();
}

Size
ClassMetadataLayout::getRelativeVTableOffset() const {
  return VTableOffset.getRelativeOffset();
}

Offset
ClassMetadataLayout::getFieldOffsetVectorOffset(IRGenFunction &IGF) const {
  return emitOffset(IGF, FieldOffsetVector);
}

Size irgen::getClassFieldOffsetOffset(IRGenModule &IGM, ClassDecl *theClass,
                                      VarDecl *field) {
  if (theClass->getForeignClassKind() == ClassDecl::ForeignKind::CFType ||
      theClass->isForeignReferenceType())
    return Size();

  return IGM.getClassMetadataLayout(theClass).getStaticFieldOffset(field);
}

/// Given a reference to class metadata of the given type,
/// compute the field offset for a stored property.
/// The type must have dependent generic layout.
llvm::Value *irgen::emitClassFieldOffset(IRGenFunction &IGF,
                                         ClassDecl *theClass,
                                         VarDecl *field,
                                         llvm::Value *metadata) {
  auto slot = emitAddressOfClassFieldOffset(IGF, metadata, theClass, field);
  return IGF.emitInvariantLoad(slot);
}

Address irgen::emitAddressOfClassFieldOffset(IRGenFunction &IGF,
                                             llvm::Value *metadata,
                                             ClassDecl *theClass,
                                             VarDecl *field) {
  auto offset =
    IGF.IGM.getClassMetadataLayout(theClass).getFieldOffset(IGF, field);
  auto slot = IGF.emitAddressAtOffset(metadata, offset, IGF.IGM.SizeTy,
                                      IGF.IGM.getPointerAlignment());
  return slot;
}

Address irgen::emitAddressOfSuperclassRefInClassMetadata(IRGenFunction &IGF,
                                                         llvm::Value *metadata) {
  // The superclass field in a class type is the first field past the isa.
  unsigned index = 1;

  Address addr(
      IGF.Builder.CreateBitCast(metadata, IGF.IGM.TypeMetadataPtrPtrTy),
      IGF.IGM.TypeMetadataPtrTy, IGF.IGM.getPointerAlignment());
  return IGF.Builder.CreateConstArrayGEP(addr, index, IGF.IGM.getPointerSize());
}

Size irgen::getStaticTupleElementOffset(IRGenModule &IGM,
                                        SILType tupleType,
                                        unsigned eltIdx) {
  assert(tupleType.is<TupleType>() && "not a tuple type");

  struct TupleElementOffsetScanner
       : LayoutScanner<TupleElementOffsetScanner, TupleMetadataScanner> {
  private:
    using super = LayoutScanner;

    // 8 seems a reasonable potential max number tuple elements to start with
    llvm::SmallVector<Size, 8> Offsets;

  public:
    TupleElementOffsetScanner(IRGenModule &IGM, TupleType *const tupleType)
      : super(IGM, tupleType) {}

    void addElement(unsigned eltIdx, const TupleTypeElt &elt) {
      Offsets.push_back(NextOffset);
      super::addElement(eltIdx, elt);
    }

    Size getElementOffset(unsigned eltIdx) const {
      return Offsets[eltIdx];
    }
  };

  TupleElementOffsetScanner s(IGM, tupleType.getAs<TupleType>().getPointer());
  s.layout();

  return s.getElementOffset(eltIdx);
}

/*********************************** ENUMS ************************************/

EnumMetadataLayout::EnumMetadataLayout(IRGenModule &IGM, EnumDecl *decl)
    : NominalMetadataLayout(Kind::Enum, decl) {

  struct Scanner : LayoutScanner<Scanner, EnumMetadataScanner> {
    using super = LayoutScanner;

    EnumMetadataLayout &Layout;
    Scanner(IRGenModule &IGM, EnumDecl *decl, EnumMetadataLayout &layout)
      : super(IGM, decl), Layout(layout) {}

    void noteStartOfTypeSpecificMembers() {
      assert(getNextOffset().getStaticOffset() ==
               IGM.getOffsetOfEnumTypeSpecificMetadataMembers());
    }

    void addPayloadSize() {
      Layout.PayloadSizeOffset = getNextOffset();
      super::addPayloadSize();
    }

    void noteStartOfGenericRequirements() {
      Layout.GenericRequirements = getNextOffset();
      super::noteStartOfGenericRequirements();
    }

    void addTrailingFlags() {
      Layout.TrailingFlagsOffset = getNextOffset();
      super::addTrailingFlags();
    }

    void layout() {
      super::layout();
      Layout.TheSize = getMetadataSize();
    }
  };

  Scanner(IGM, decl, *this).layout();
}

Offset
EnumMetadataLayout::getPayloadSizeOffset() const {
  assert(PayloadSizeOffset.isStatic());
  return Offset(PayloadSizeOffset.getStaticOffset());
}

Offset EnumMetadataLayout::getTrailingFlagsOffset() const {
  assert(TrailingFlagsOffset.isStatic());
  return Offset(TrailingFlagsOffset.getStaticOffset());
}

/********************************** STRUCTS ***********************************/

StructMetadataLayout::StructMetadataLayout(IRGenModule &IGM, StructDecl *decl)
    : NominalMetadataLayout(Kind::Struct, decl) {

  struct Scanner : LayoutScanner<Scanner, StructMetadataScanner> {
    using super = LayoutScanner;

    StructMetadataLayout &Layout;
    Scanner(IRGenModule &IGM, StructDecl *decl, StructMetadataLayout &layout)
      : super(IGM, decl), Layout(layout) {}

    void noteStartOfTypeSpecificMembers() {
      assert(getNextOffset().getStaticOffset() ==
               IGM.getOffsetOfStructTypeSpecificMetadataMembers());
    }

    void noteStartOfGenericRequirements() {
      Layout.GenericRequirements = getNextOffset();
      super::noteStartOfGenericRequirements();
    }

    void noteStartOfFieldOffsets() {
      Layout.FieldOffsetVector = getNextOffset();
      super::noteStartOfFieldOffsets();
    }

    void addFieldOffset(VarDecl *field) {
      Layout.FieldOffsets.try_emplace(field, getNextOffset());
      super::addFieldOffset(field);
    }

    void noteEndOfFieldOffsets() {
      super::noteEndOfFieldOffsets();
    }

    void addTrailingFlags() {
      Layout.TrailingFlagsOffset = getNextOffset();
      super::addTrailingFlags();
    }

    void layout() {
      super::layout();
      Layout.TheSize = getMetadataSize();
    }
  };

  Scanner(IGM, decl, *this).layout();
}

Offset StructMetadataLayout::getFieldOffset(IRGenFunction &IGF,
                                            VarDecl *field) const {
  // TODO: implement resilient metadata layout
  return Offset(getStaticFieldOffset(field));
}
Size StructMetadataLayout::getStaticFieldOffset(VarDecl *field) const {
  auto &stored = getStoredFieldOffset(field);
  assert(stored.isStatic() && "resilient struct metadata layout unsupported!");
  return stored.getStaticOffset();
}

Offset
StructMetadataLayout::getFieldOffsetVectorOffset() const {
  assert(FieldOffsetVector.isStatic());
  return Offset(FieldOffsetVector.getStaticOffset());
}

Offset 
StructMetadataLayout::getTrailingFlagsOffset() const {
  assert(TrailingFlagsOffset.isStatic());
  return Offset(TrailingFlagsOffset.getStaticOffset());
}
/****************************** FOREIGN CLASSES *******************************/
ForeignClassMetadataLayout::ForeignClassMetadataLayout(IRGenModule &IGM,
                                                       ClassDecl *theClass)
    : MetadataLayout(Kind::ForeignClass), Class(theClass) {
  assert(theClass->getForeignClassKind() == ClassDecl::ForeignKind::CFType ||
         theClass->isForeignReferenceType() &&
         "Not a foreign class");

  struct Scanner : LayoutScanner<Scanner, ForeignClassMetadataScanner> {
    using super = LayoutScanner;

    ForeignClassMetadataLayout &Layout;
    Scanner(IRGenModule &IGM, ClassDecl *decl,
            ForeignClassMetadataLayout &layout)
      : super(IGM, decl), Layout(layout) {}

    void addSuperclass() {
      Layout.SuperClassOffset = getNextOffset();
      super::addSuperclass();
    }

    void layout() {
      super::layout();
      Layout.TheSize = getMetadataSize();
    }
  };

  Scanner(IGM, theClass, *this).layout();

}

//===--- DebugTypeInfo.cpp - Type Info for Debugging ----------------------===//
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
// This file defines the data structure that holds all the debug info
// we want to emit for types.
//
//===----------------------------------------------------------------------===//

#include "DebugTypeInfo.h"
#include "FixedTypeInfo.h"
#include "IRGenModule.h"
#include "swift/SIL/SILGlobalVariable.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;
using namespace irgen;

DebugTypeInfo::DebugTypeInfo(swift::Type Ty, llvm::Type *FragmentStorageTy,
                             llvm::Optional<Size::int_type> SizeInBits,
                             Alignment Align, bool HasDefaultAlignment,
                             bool IsMetadata, bool SizeIsFragmentSize,
                             bool IsFixedBuffer)
    : Type(Ty.getPointer()), FragmentStorageType(FragmentStorageTy),
      SizeInBits(SizeInBits), Align(Align),
      DefaultAlignment(HasDefaultAlignment), IsMetadataType(IsMetadata),
      SizeIsFragmentSize(SizeIsFragmentSize), IsFixedBuffer(IsFixedBuffer) {
  assert(Align.getValue() != 0);
}

/// Determine whether this type has an attribute specifying a custom alignment.
static bool hasDefaultAlignment(swift::Type Ty) {
  if (auto CanTy = Ty->getCanonicalType())
    if (auto *TyDecl = CanTy.getNominalOrBoundGenericNominal())
      if (TyDecl->getAttrs().getAttribute<AlignmentAttr>()
          || TyDecl->getAttrs().getAttribute<RawLayoutAttr>())
        return false;
  return true;
}

DebugTypeInfo DebugTypeInfo::getFromTypeInfo(swift::Type Ty, const TypeInfo &TI,
                                             IRGenModule &IGM,
                                             bool IsFragmentTypeInfo) {
  llvm::Optional<Size::int_type> SizeInBits;
  llvm::Type *StorageType = TI.getStorageType();
  if (StorageType->isSized())
    SizeInBits = IGM.DataLayout.getTypeSizeInBits(StorageType);
  else if (TI.isFixedSize()) {
    const FixedTypeInfo &FixTy = *cast<const FixedTypeInfo>(&TI);
    Size::int_type Size = FixTy.getFixedSize().getValue() * 8;
    SizeInBits = Size;
  }
  assert(TI.getStorageType() && "StorageType is a nullptr");
  return DebugTypeInfo(Ty.getPointer(), StorageType, SizeInBits,
                       TI.getBestKnownAlignment(), ::hasDefaultAlignment(Ty),
                       false, IsFragmentTypeInfo);
}

DebugTypeInfo DebugTypeInfo::getLocalVariable(VarDecl *Decl, swift::Type Ty,
                                              const TypeInfo &Info,
                                              IRGenModule &IGM,
                                              bool IsFragmentTypeInfo) {

  auto DeclType = Decl->getInterfaceType();
  auto RealType = Ty;

  // DynamicSelfType is also sugar as far as debug info is concerned.
  auto Sugared = DeclType;
  if (auto DynSelfTy = DeclType->getAs<DynamicSelfType>())
    Sugared = DynSelfTy->getSelfType();

  // Prefer the original, potentially sugared version of the type if
  // the type hasn't been mucked with by an optimization pass.
  auto *Type = Sugared->isEqual(RealType) ? DeclType.getPointer()
                                          : RealType.getPointer();
  return getFromTypeInfo(Type, Info, IGM, IsFragmentTypeInfo);
}

DebugTypeInfo DebugTypeInfo::getGlobalMetadata(swift::Type Ty,
                                               llvm::Type *StorageTy, Size size,
                                               Alignment align) {
  DebugTypeInfo DbgTy(Ty.getPointer(), StorageTy, size.getValue() * 8, align,
                      true, false, false);
  assert(StorageTy && "StorageType is a nullptr");
  assert(!DbgTy.isContextArchetype() &&
         "type metadata cannot contain an archetype");
  return DbgTy;
}

DebugTypeInfo DebugTypeInfo::getTypeMetadata(swift::Type Ty,
                                             llvm::Type *StorageTy, Size size,
                                             Alignment align) {
  DebugTypeInfo DbgTy(Ty.getPointer(), StorageTy, size.getValue() * 8, align,
                      true, true, false);
  assert(StorageTy && "StorageType is a nullptr");
  assert(!DbgTy.isContextArchetype() &&
         "type metadata cannot contain an archetype");
  return DbgTy;
}

DebugTypeInfo DebugTypeInfo::getForwardDecl(swift::Type Ty) {
  DebugTypeInfo DbgTy(Ty.getPointer());
  return DbgTy;
}

DebugTypeInfo DebugTypeInfo::getGlobal(SILGlobalVariable *GV,
                                       llvm::Type *FragmentStorageType,
                                       IRGenModule &IGM) {
  // Prefer the original, potentially sugared version of the type if
  // the type hasn't been mucked with by an optimization pass.
  auto LowTy = GV->getLoweredType().getASTType();
  auto *Type = LowTy.getPointer();
  if (auto *Decl = GV->getDecl()) {
    auto DeclType = Decl->getTypeInContext();
    if (DeclType->isEqual(LowTy))
      Type = DeclType.getPointer();
  }
  auto &TI = IGM.getTypeInfoForUnlowered(Type);
  DebugTypeInfo DbgTy = getFromTypeInfo(Type, TI, IGM, false);
  assert(FragmentStorageType && "FragmentStorageType is a nullptr");
  assert(!DbgTy.isContextArchetype() &&
         "type of global variable cannot be an archetype");
  return DbgTy;
}

DebugTypeInfo
DebugTypeInfo::getGlobalFixedBuffer(SILGlobalVariable *GV,
                                    llvm::Type *FragmentStorageType,
                                    Size SizeInBytes, Alignment Align) {
  // Prefer the original, potentially sugared version of the type if
  // the type hasn't been mucked with by an optimization pass.
  auto LowTy = GV->getLoweredType().getASTType();
  auto *Type = LowTy.getPointer();
  if (auto *Decl = GV->getDecl()) {
    auto DeclType = Decl->getTypeInContext();
    if (DeclType->isEqual(LowTy))
      Type = DeclType.getPointer();
  }
  DebugTypeInfo DbgTy(Type, FragmentStorageType, SizeInBytes.getValue() * 8,
                      Align, ::hasDefaultAlignment(Type), false, false, true);
  assert(FragmentStorageType && "FragmentStorageType is a nullptr");
  assert(!DbgTy.isContextArchetype() &&
         "type of global variable cannot be an archetype");
  return DbgTy;
}

DebugTypeInfo DebugTypeInfo::getObjCClass(ClassDecl *theClass,
                                          llvm::Type *FragmentStorageType,
                                          Size SizeInBytes, Alignment align) {
  DebugTypeInfo DbgTy(theClass->getInterfaceType().getPointer(),
                      FragmentStorageType, SizeInBytes.getValue() * 8, align,
                      true, false, false);
  assert(FragmentStorageType && "FragmentStorageType is a nullptr");
  assert(!DbgTy.isContextArchetype() &&
         "type of objc class cannot be an archetype");
  return DbgTy;
}

DebugTypeInfo DebugTypeInfo::getErrorResult(swift::Type Ty,
                                            IRGenModule &IGM) {
  auto &TI = IGM.getTypeInfoForUnlowered(Ty);
  DebugTypeInfo DbgTy = getFromTypeInfo(Ty, TI, IGM, false);
  return DbgTy;
}

bool DebugTypeInfo::operator==(DebugTypeInfo T) const {
  return getType() == T.getType() &&
         SizeInBits == T.SizeInBits &&
         Align == T.Align;
}

bool DebugTypeInfo::operator!=(DebugTypeInfo T) const { return !operator==(T); }

TypeDecl *DebugTypeInfo::getDecl() const {
  if (auto *N = dyn_cast<NominalType>(Type))
    return N->getDecl();
  if (auto *BTA = dyn_cast<TypeAliasType>(Type))
    return BTA->getDecl();
  if (auto *UBG = dyn_cast<UnboundGenericType>(Type))
    return UBG->getDecl();
  if (auto *BG = dyn_cast<BoundGenericType>(Type))
    return BG->getDecl();
  if (auto *E = dyn_cast<ExistentialType>(Type))
    return E->getConstraintType()->getAnyNominal();
  return nullptr;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void DebugTypeInfo::dump() const {
  llvm::errs() << "[";
  if (SizeInBits)
    llvm::errs() << "SizeInBits " << *SizeInBits << " ";
  llvm::errs() << "Alignment " << Align.getValue() << "] ";
  if (auto *Type = getType())
    Type->dump(llvm::errs());

  if (FragmentStorageType) {
    llvm::errs() << "FragmentStorageType=";
    FragmentStorageType->dump();
  } else
    llvm::errs() << "forward-declared\n";
}
#endif

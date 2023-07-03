//===--- GenStruct.cpp - Swift IR Generation For 'struct' Types -----------===//
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
//  This file implements IR generation for struct types.
//
//===----------------------------------------------------------------------===//

#include "GenStruct.h"

#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/Decl.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/Types.h"
#include "swift/IRGen/Linking.h"
#include "swift/SIL/SILFunctionBuilder.h"
#include "swift/SIL/SILModule.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/RecordLayout.h"
#include "clang/CodeGen/CodeGenABITypes.h"
#include "clang/CodeGen/SwiftCallingConv.h"
#include "clang/Sema/Sema.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"

#include "GenDecl.h"
#include "GenMeta.h"
#include "GenRecord.h"
#include "GenType.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "IndirectTypeInfo.h"
#include "MemberAccessStrategy.h"
#include "MetadataLayout.h"
#include "NonFixedTypeInfo.h"
#include "ResilientTypeInfo.h"
#include "Signature.h"
#include "StructMetadataVisitor.h"

#pragma clang diagnostic ignored "-Winconsistent-missing-override"

using namespace swift;
using namespace irgen;

/// The kinds of TypeInfos implementing struct types.
enum class StructTypeInfoKind {
  LoadableStructTypeInfo,
  FixedStructTypeInfo,
  LoadableClangRecordTypeInfo,
  AddressOnlyClangRecordTypeInfo,
  NonFixedStructTypeInfo,
  ResilientStructTypeInfo
};

static StructTypeInfoKind getStructTypeInfoKind(const TypeInfo &type) {
  return (StructTypeInfoKind) type.getSubclassKind();
}

/// If this type has a CXXDestructorDecl, find it and return it. Otherwise,
/// return nullptr.
static clang::CXXDestructorDecl *getCXXDestructor(SILType type) {
  auto *structDecl = type.getStructOrBoundGenericStruct();
  if (!structDecl || !structDecl->getClangDecl())
    return nullptr;
  const clang::CXXRecordDecl *cxxRecordDecl =
      dyn_cast<clang::CXXRecordDecl>(structDecl->getClangDecl());
  if (!cxxRecordDecl)
    return nullptr;
  return cxxRecordDecl->getDestructor();
}
namespace {
  class StructFieldInfo : public RecordField<StructFieldInfo> {
  public:
    StructFieldInfo(VarDecl *field, const TypeInfo &type)
      : RecordField(type), Field(field) {}

    /// The field.
    VarDecl * const Field;

    StringRef getFieldName() const {
      return Field->getName().str();
    }

    SILType getType(IRGenModule &IGM, SILType T) const {
      return T.getFieldType(Field, IGM.getSILModule(),
                            IGM.getMaximalTypeExpansionContext());
    }
  };

  /// A field-info implementation for fields of Clang types.
  class ClangFieldInfo : public RecordField<ClangFieldInfo> {
  public:
    ClangFieldInfo(VarDecl *swiftField, const ElementLayout &layout,
                   const TypeInfo &typeInfo)
        : RecordField(typeInfo), Field(swiftField) {
      completeFrom(layout);
    }

    ClangFieldInfo(VarDecl *swiftField, const ElementLayout &layout,
                   unsigned explosionBegin, unsigned explosionEnd)
      : RecordField(layout, explosionBegin, explosionEnd),
        Field(swiftField) {}

    VarDecl *Field;

    StringRef getFieldName() const {
      if (Field) return Field->getName().str();
      return "<unimported>";
    }

    SILType getType(IRGenModule &IGM, SILType T) const {
      if (Field)
        return T.getFieldType(Field, IGM.getSILModule(),
                              IGM.getMaximalTypeExpansionContext());

      // The Swift-field-less cases use opaque storage, which is
      // guaranteed to ignore the type passed to it.
      return {};
    }
  };

  /// A common base class for structs.
  template <class Impl, class Base, class FieldInfoType = StructFieldInfo>
  class StructTypeInfoBase :
     public RecordTypeInfo<Impl, Base, FieldInfoType> {
    using super = RecordTypeInfo<Impl, Base, FieldInfoType>;
  protected:
    template <class... As>
    StructTypeInfoBase(StructTypeInfoKind kind, As &&...args)
      : super(std::forward<As>(args)...) {
      super::setSubclassKind((unsigned) kind);
    }

    using super::asImpl;

  public:
    const FieldInfoType &getFieldInfo(VarDecl *field) const {
      // FIXME: cache the physical field index in the VarDecl.
      for (auto &fieldInfo : asImpl().getFields()) {
        if (fieldInfo.Field == field)
          return fieldInfo;
      }
      llvm_unreachable("field not in struct?");
    }

    /// Given a full struct explosion, project out a single field.
    virtual void projectFieldFromExplosion(IRGenFunction &IGF, Explosion &in,
                                           VarDecl *field,
                                           Explosion &out) const {
      auto &fieldInfo = getFieldInfo(field);

      // If the field requires no storage, there's nothing to do.
      if (fieldInfo.isEmpty())
        return;

      // Otherwise, project from the base.
      auto fieldRange = fieldInfo.getProjectionRange();
      auto elements = in.getRange(fieldRange.first, fieldRange.second);
      out.add(elements);
    }

    /// Given the address of a struct value, project out the address of a
    /// single field.
    Address projectFieldAddress(IRGenFunction &IGF, Address addr, SILType T,
                                const FieldInfoType &field) const {
      return asImpl().projectFieldAddress(IGF, addr, T, field.Field);
    }

    /// Given the address of a struct value, project out the address of a
    /// single field.
    Address projectFieldAddress(IRGenFunction &IGF, Address addr, SILType T,
                                VarDecl *field) const {
      auto &fieldInfo = getFieldInfo(field);
      if (fieldInfo.isEmpty())
        return fieldInfo.getTypeInfo().getUndefAddress();

      auto offsets = asImpl().getNonFixedOffsets(IGF, T);
      return fieldInfo.projectAddress(IGF, addr, offsets);
    }

    /// Return the constant offset of a field as a Int32Ty, or nullptr if the
    /// field is not at a fixed offset.
    llvm::Constant *getConstantFieldOffset(IRGenModule &IGM,
                                           VarDecl *field) const {
      auto &fieldInfo = getFieldInfo(field);
      if (fieldInfo.hasFixedByteOffset()) {
        return llvm::ConstantInt::get(
            IGM.Int32Ty, fieldInfo.getFixedByteOffset().getValue());
      }
      return nullptr;
    }

    const TypeInfo *getFieldTypeInfo(IRGenModule &IGM, VarDecl *field) const {
      auto &fieldInfo = getFieldInfo(field);
      if (fieldInfo.isEmpty())
        return nullptr;
      return &fieldInfo.getTypeInfo();
    }

    MemberAccessStrategy getFieldAccessStrategy(IRGenModule &IGM,
                                             SILType T, VarDecl *field) const {
      auto &fieldInfo = getFieldInfo(field);
      switch (fieldInfo.getKind()) {
      case ElementLayout::Kind::Fixed:
      case ElementLayout::Kind::Empty:
      case ElementLayout::Kind::EmptyTailAllocatedCType:
        return MemberAccessStrategy::getDirectFixed(
                                               fieldInfo.getFixedByteOffset());
      case ElementLayout::Kind::InitialNonFixedSize:
        return MemberAccessStrategy::getDirectFixed(Size(0));
      case ElementLayout::Kind::NonFixed:
        return asImpl().getNonFixedFieldAccessStrategy(IGM, T, fieldInfo);
      }
      llvm_unreachable("bad field layout kind");
    }

    unsigned getFieldIndex(IRGenModule &IGM, VarDecl *field) const {
      auto &fieldInfo = getFieldInfo(field);
      return fieldInfo.getStructIndex();
    }

    llvm::Optional<unsigned> getFieldIndexIfNotEmpty(IRGenModule &IGM,
                                                     VarDecl *field) const {
      auto &fieldInfo = getFieldInfo(field);
      if (fieldInfo.isEmpty())
        return llvm::None;
      return fieldInfo.getStructIndex();
    }

    bool isSingleRetainablePointer(ResilienceExpansion expansion,
                                   ReferenceCounting *rc) const override {
      // If the type isn't copyable, it doesn't share representation with
      // a single-refcounted pointer.
      //
      // This is sufficient to rule out types with user-defined deinits today,
      // since copyable structs are not allowed to define a deinit. If we
      // ever added user-defined copy constructors to the language, then we'd
      // have to also check that.
      if (!this->isCopyable(expansion)) {
        return false;
      }
                                   
      auto fields = asImpl().getFields();
      if (fields.size() != 1)
        return false;
      return fields[0].getTypeInfo().isSingleRetainablePointer(expansion, rc);
    }

    void destroy(IRGenFunction &IGF, Address address, SILType T,
                 bool isOutlined) const override {
      // If the struct has a deinit declared, then call it to destroy the
      // value.
      if (tryEmitDestroyUsingDeinit(IGF, address, T)) {
        return;
      }
      
      // Otherwise, perform elementwise destruction of the value.
      return super::destroy(IGF, address, T, isOutlined);
    }

    void verify(IRGenTypeVerifierFunction &IGF,
                llvm::Value *metadata,
                SILType structType) const override {
      // Check that constant field offsets we know match
      for (auto &field : asImpl().getFields()) {
        switch (field.getKind()) {
        case ElementLayout::Kind::Fixed: {
          // We know the offset at compile time. See whether there's also an
          // entry for this field in the field offset vector.
          class FindOffsetOfFieldOffsetVector
            : public StructMetadataScanner<FindOffsetOfFieldOffsetVector> {
          public:
            VarDecl *FieldToFind;
            Size AddressPoint = Size::invalid();
            Size FieldOffset = Size::invalid();

            FindOffsetOfFieldOffsetVector(IRGenModule &IGM, VarDecl *Field)
                : StructMetadataScanner<FindOffsetOfFieldOffsetVector>(
                      IGM, cast<StructDecl>(Field->getDeclContext())),
                  FieldToFind(Field) {}

            void noteAddressPoint() {
              AddressPoint = this->NextOffset;
            }

            void addFieldOffset(VarDecl *Field) {
              if (Field == FieldToFind) {
                FieldOffset = this->NextOffset;
              }
              StructMetadataScanner<
                  FindOffsetOfFieldOffsetVector>::addFieldOffset(Field);
            }
          };

          FindOffsetOfFieldOffsetVector scanner(IGF.IGM, field.Field);
          scanner.layout();

          if (scanner.FieldOffset == Size::invalid()
              || scanner.AddressPoint == Size::invalid())
            continue;

          // Load the offset from the field offset vector and ensure it matches
          // the compiler's idea of the offset.
          auto metadataBytes =
            IGF.Builder.CreateBitCast(metadata, IGF.IGM.Int8PtrTy);
          auto fieldOffsetPtr = IGF.Builder.CreateInBoundsGEP(
              IGF.IGM.Int8Ty, metadataBytes,
              IGF.IGM.getSize(scanner.FieldOffset - scanner.AddressPoint));
          fieldOffsetPtr =
            IGF.Builder.CreateBitCast(fieldOffsetPtr,
                                      IGF.IGM.Int32Ty->getPointerTo());
          llvm::Value *fieldOffset = IGF.Builder.CreateLoad(
              Address(fieldOffsetPtr, IGF.IGM.Int32Ty, Alignment(4)));
          fieldOffset = IGF.Builder.CreateZExtOrBitCast(fieldOffset,
                                                        IGF.IGM.SizeTy);

          IGF.verifyValues(metadata, fieldOffset,
                       IGF.IGM.getSize(field.getFixedByteOffset()),
                       Twine("offset of struct field ") + field.getFieldName());
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

  /// A type implementation for loadable record types imported from Clang.
  class LoadableClangRecordTypeInfo final
      : public StructTypeInfoBase<LoadableClangRecordTypeInfo, LoadableTypeInfo,
                                  ClangFieldInfo> {
    const clang::RecordDecl *ClangDecl;

    template <class Fn>
    void forEachNonEmptyBase(Fn fn) const {
      auto &layout = ClangDecl->getASTContext().getASTRecordLayout(ClangDecl);

      if (auto cxxRecord = dyn_cast<clang::CXXRecordDecl>(ClangDecl)) {
        for (auto base : cxxRecord->bases()) {
          auto baseType = base.getType().getCanonicalType();

          auto baseRecord = cast<clang::RecordType>(baseType)->getDecl();
          auto baseCxxRecord = cast<clang::CXXRecordDecl>(baseRecord);

          if (baseCxxRecord->isEmpty())
            continue;

          auto offset = layout.getBaseClassOffset(baseCxxRecord);
          auto size =
              ClangDecl->getASTContext().getTypeSizeInChars(baseType);
          fn(baseType, offset, size);
        }
      }
    }

  public:
    LoadableClangRecordTypeInfo(ArrayRef<ClangFieldInfo> fields,
                                unsigned explosionSize, llvm::Type *storageType,
                                Size size, SpareBitVector &&spareBits,
                                Alignment align,
                                const clang::RecordDecl *clangDecl)
        : StructTypeInfoBase(StructTypeInfoKind::LoadableClangRecordTypeInfo,
                             fields, explosionSize, storageType, size,
                             std::move(spareBits), align,
                             IsTriviallyDestroyable,
                             IsCopyable,
                             IsFixedSize),
          ClangDecl(clangDecl) {}

    TypeLayoutEntry
    *buildTypeLayoutEntry(IRGenModule &IGM,
                          SILType T,
                          bool useStructLayouts) const override {
      if (!useStructLayouts) {
        return IGM.typeLayoutCache.getOrCreateTypeInfoBasedEntry(*this, T);
      }
      if (!areFieldsABIAccessible()) {
        return IGM.typeLayoutCache.getOrCreateResilientEntry(T);
      }
      if (getFields().empty()) {
        return IGM.typeLayoutCache.getEmptyEntry();
      }

      std::vector<TypeLayoutEntry *> fields;
      for (auto &field : getFields()) {
        auto fieldTy = field.getType(IGM, T);
        if (!fieldTy) {
          return IGM.typeLayoutCache.getOrCreateTypeInfoBasedEntry(*this, T);
        }
        fields.push_back(
            field.getTypeInfo().buildTypeLayoutEntry(IGM, fieldTy, useStructLayouts));
      }
      assert(!fields.empty() &&
             "Empty structs should not be LoadableClangRecordTypeInfo");

      // if (fields.size() == 1 && getBestKnownAlignment() == *fields[0]->fixedAlignment(IGM)) {
      //   return fields[0];
      // }

      return IGM.typeLayoutCache.getOrCreateAlignedGroupEntry(
          fields, T, getBestKnownAlignment().getValue(), *this);
    }

    void initializeFromParams(IRGenFunction &IGF, Explosion &params,
                              Address addr, SILType T,
                              bool isOutlined) const override {
      LoadableClangRecordTypeInfo::initialize(IGF, params, addr, isOutlined);
    }

    void addToAggLowering(IRGenModule &IGM, SwiftAggLowering &lowering,
                          Size offset) const override {
      forEachNonEmptyBase([&](clang::QualType type, clang::CharUnits offset,
                              clang::CharUnits) {
        lowering.addTypedData(type, offset);
      });

      lowering.addTypedData(ClangDecl, offset.asCharUnits());
    }

    llvm::NoneType getNonFixedOffsets(IRGenFunction &IGF) const {
      return llvm::None;
    }
    llvm::NoneType getNonFixedOffsets(IRGenFunction &IGF, SILType T) const {
      return llvm::None;
    }
    MemberAccessStrategy
    getNonFixedFieldAccessStrategy(IRGenModule &IGM, SILType T,
                                   const ClangFieldInfo &field) const {
      llvm_unreachable("non-fixed field in Clang type?");
    }
  };

  class AddressOnlyPointerAuthRecordTypeInfo final
      : public StructTypeInfoBase<AddressOnlyPointerAuthRecordTypeInfo,
                                  FixedTypeInfo, ClangFieldInfo> {
    const clang::RecordDecl *clangDecl;

    void emitCopyWithCopyFunction(IRGenFunction &IGF, SILType T, Address src,
                                  Address dst) const {
      auto *copyFunction =
          clang::CodeGen::getNonTrivialCStructCopyAssignmentOperator(
              IGF.IGM.getClangCGM(), dst.getAlignment(), src.getAlignment(),
              /*isVolatile*/ false,
              clang::QualType(clangDecl->getTypeForDecl(), 0));
      auto *dstValue = dst.getAddress();
      auto *srcValue = src.getAddress();
      if (IGF.IGM.getLLVMContext().supportsTypedPointers()) {
        dstValue = IGF.coerceValue(
            dst.getAddress(), copyFunction->getFunctionType()->getParamType(0),
            IGF.IGM.DataLayout);
        srcValue = IGF.coerceValue(
            src.getAddress(), copyFunction->getFunctionType()->getParamType(1),
            IGF.IGM.DataLayout);
      }
      IGF.Builder.CreateCall(copyFunction->getFunctionType(), copyFunction,
                             {dstValue, srcValue});
    }

  public:
    AddressOnlyPointerAuthRecordTypeInfo(ArrayRef<ClangFieldInfo> fields,
                                         llvm::Type *storageType, Size size,
                                         Alignment align,
                                         const clang::RecordDecl *clangDecl)
        : StructTypeInfoBase(StructTypeInfoKind::AddressOnlyClangRecordTypeInfo,
                             fields, storageType, size,
                             // We can't assume any spare bits in a C++ type
                             // with user-defined special member functions.
                             SpareBitVector(llvm::Optional<APInt>{
                                 llvm::APInt(size.getValueInBits(), 0)}),
                             align, IsNotTriviallyDestroyable,
                             IsNotBitwiseTakable,
                             IsCopyable,
                             IsFixedSize),
          clangDecl(clangDecl) {
      (void)clangDecl;
    }

    TypeLayoutEntry
    *buildTypeLayoutEntry(IRGenModule &IGM,
                          SILType T,
                          bool useStructLayouts) const override {
      if (!useStructLayouts) {
        return IGM.typeLayoutCache.getOrCreateTypeInfoBasedEntry(*this, T);
      }
      assert(false && "Implement proper type layout info in the future");
      return IGM.typeLayoutCache.getOrCreateTypeInfoBasedEntry(*this, T);
    }

    void initializeFromParams(IRGenFunction &IGF, Explosion &params,
                              Address addr, SILType T,
                              bool isOutlined) const override {
      llvm_unreachable("Address-only C++ types must be created by C++ special "
                       "member functions.");
    }

    void initializeWithCopy(IRGenFunction &IGF, Address dst, Address src,
                            SILType T, bool isOutlined) const override {
      emitCopyWithCopyFunction(IGF, T, src, dst);
    }

    void assignWithCopy(IRGenFunction &IGF, Address dst, Address src, SILType T,
                        bool isOutlined) const override {
      emitCopyWithCopyFunction(IGF, T, src, dst);
    }

    void initializeWithTake(IRGenFunction &IGF, Address dst, Address src,
                            SILType T, bool isOutlined) const override {
      emitCopyWithCopyFunction(IGF, T, src, dst);
      destroy(IGF, src, T, isOutlined);
    }

    void assignWithTake(IRGenFunction &IGF, Address dst, Address src, SILType T,
                        bool isOutlined) const override {
      emitCopyWithCopyFunction(IGF, T, src, dst);
      destroy(IGF, src, T, isOutlined);
    }

    llvm::NoneType getNonFixedOffsets(IRGenFunction &IGF) const {
      return llvm::None;
    }
    llvm::NoneType getNonFixedOffsets(IRGenFunction &IGF, SILType T) const {
      return llvm::None;
    }
    MemberAccessStrategy
    getNonFixedFieldAccessStrategy(IRGenModule &IGM, SILType T,
                                   const ClangFieldInfo &field) const {
      llvm_unreachable("non-fixed field in Clang type?");
    }
  };

  class AddressOnlyCXXClangRecordTypeInfo final
      : public StructTypeInfoBase<AddressOnlyCXXClangRecordTypeInfo,
                                  FixedTypeInfo, ClangFieldInfo> {
    const clang::RecordDecl *ClangDecl;

    const clang::CXXConstructorDecl *findCopyConstructor() const {
      const clang::CXXRecordDecl *cxxRecordDecl =
          dyn_cast<clang::CXXRecordDecl>(ClangDecl);
      if (!cxxRecordDecl)
        return nullptr;
      for (auto method : cxxRecordDecl->methods()) {
        if (auto ctor = dyn_cast<clang::CXXConstructorDecl>(method)) {
          if (ctor->isCopyConstructor() &&
              ctor->getAccess() == clang::AS_public &&
              // rdar://106964356
              // ctor->doesThisDeclarationHaveABody() &&
              !ctor->isDeleted())
            return ctor;
        }
      }
      return nullptr;
    }

    const clang::CXXConstructorDecl *findMoveConstructor() const {
      const clang::CXXRecordDecl *cxxRecordDecl =
          dyn_cast<clang::CXXRecordDecl>(ClangDecl);
      if (!cxxRecordDecl)
        return nullptr;
      for (auto method : cxxRecordDecl->methods()) {
        if (auto ctor = dyn_cast<clang::CXXConstructorDecl>(method)) {
          if (ctor->isMoveConstructor() &&
              ctor->getAccess() == clang::AS_public &&
              ctor->doesThisDeclarationHaveABody() &&
              !ctor->isDeleted())
            return ctor;
        }
      }
      return nullptr;
    }

    CanSILFunctionType createCXXCopyConstructorFunctionType(IRGenFunction &IGF,
                                                            SILType T) const {
      // Create the following function type:
      //   @convention(c) (UnsafePointer<T>) -> @out T
      // This is how clang *would* import the copy constructor. So, later, when
      // we pass it to "emitCXXConstructorThunkIfNeeded" we get a thunk with
      // the following LLVM function type:
      //   void (%struct.T* %this, %struct.T* %0)
      auto ptrTypeDecl =
          IGF.getSILModule().getASTContext().getUnsafePointerDecl();
      auto subst = SubstitutionMap::get(ptrTypeDecl->getGenericSignature(),
                                        {T.getASTType()},
                                        ArrayRef<ProtocolConformanceRef>{});
      auto ptrType = ptrTypeDecl->getDeclaredInterfaceType().subst(subst);
      SILParameterInfo ptrParam(ptrType->getCanonicalType(),
                                ParameterConvention::Direct_Unowned);
      SILResultInfo result(T.getASTType(), ResultConvention::Indirect);

      return SILFunctionType::get(
          GenericSignature(),
          SILFunctionType::ExtInfo().withRepresentation(
              SILFunctionTypeRepresentation::CFunctionPointer),
          SILCoroutineKind::None,
          /*callee=*/ParameterConvention::Direct_Unowned,
          /*params*/ {ptrParam},
          /*yields*/ {}, /*results*/ {result},
          /*error*/ llvm::None,
          /*pattern subs*/ SubstitutionMap(),
          /*invocation subs*/ SubstitutionMap(), IGF.IGM.Context);
    }

    void emitCopyWithCopyConstructor(
        IRGenFunction &IGF, SILType T,
        const clang::CXXConstructorDecl *copyConstructor, llvm::Value *src,
        llvm::Value *dest) const {
      auto fnType = createCXXCopyConstructorFunctionType(IGF, T);
      auto globalDecl =
          clang::GlobalDecl(copyConstructor, clang::Ctor_Complete);
      auto clangFnAddr =
          IGF.IGM.getAddrOfClangGlobalDecl(globalDecl, NotForDefinition);
      auto callee = cast<llvm::Function>(clangFnAddr->stripPointerCasts());
      Signature signature = IGF.IGM.getSignature(fnType);
      std::string name = "__swift_cxx_copy_ctor" + callee->getName().str();
      auto *origClangFnAddr = clangFnAddr;
      clangFnAddr = emitCXXConstructorThunkIfNeeded(
          IGF.IGM, signature, copyConstructor, name, clangFnAddr);
      callee = cast<llvm::Function>(clangFnAddr);
      if (IGF.IGM.getLLVMContext().supportsTypedPointers()) {
        dest = IGF.coerceValue(dest, callee->getFunctionType()->getParamType(0),
                               IGF.IGM.DataLayout);
        src = IGF.coerceValue(src, callee->getFunctionType()->getParamType(1),
                              IGF.IGM.DataLayout);
      }
      llvm::Value *args[] = {dest, src};
      if (clangFnAddr == origClangFnAddr) {
        // Ensure we can use 'invoke' to trap on uncaught exceptions when
        // calling original copy constructor without going through the thunk.
        emitCXXConstructorCall(IGF, copyConstructor, callee->getFunctionType(),
                               callee, args);
        return;
      }
      // Check if we're calling a thunk that traps on exception thrown from copy
      // constructor.
      if (IGF.IGM.emittedForeignFunctionThunksWithExceptionTraps.count(callee))
        IGF.setCallsThunksWithForeignExceptionTraps();
      IGF.Builder.CreateCall(callee->getFunctionType(), callee, args);
    }

  public:
    AddressOnlyCXXClangRecordTypeInfo(ArrayRef<ClangFieldInfo> fields,
                                      llvm::Type *storageType, Size size,
                                      Alignment align,
                                      const clang::RecordDecl *clangDecl)
        : StructTypeInfoBase(StructTypeInfoKind::AddressOnlyClangRecordTypeInfo,
                             fields, storageType, size,
                             // We can't assume any spare bits in a C++ type
                             // with user-defined special member functions.
                             SpareBitVector(llvm::Optional<APInt>{
                                 llvm::APInt(size.getValueInBits(), 0)}),
                             align, IsNotTriviallyDestroyable, IsNotBitwiseTakable,
                             // TODO: Set this appropriately for the type's
                             // C++ import behavior.
                             IsCopyable,
                             IsFixedSize),
          ClangDecl(clangDecl) {
      (void)ClangDecl;
    }

    void destroy(IRGenFunction &IGF, Address address, SILType T,
                 bool isOutlined) const override {
      auto *destructor = getCXXDestructor(T);
      // If the destructor is trivial, clang will assert when we call
      // `emitCXXDestructorCall` so, just let Swift handle this destructor.
      if (!destructor || destructor->isTrivial()) {
        // If we didn't find a destructor to call, bail out to the parent
        // implementation.
        StructTypeInfoBase<AddressOnlyCXXClangRecordTypeInfo, FixedTypeInfo,
                           ClangFieldInfo>::destroy(IGF, address, T,
                                                    isOutlined);
        return;
      }

      if (!destructor->isUserProvided() &&
          !destructor->doesThisDeclarationHaveABody()) {
        assert(!destructor->isDeleted() &&
               "Swift cannot handle a type with no known destructor.");
        // Make sure we define the destructor so we have something to call.
        auto &sema = IGF.IGM.Context.getClangModuleLoader()->getClangSema();
        sema.DefineImplicitDestructor(clang::SourceLocation(), destructor);
      }

      clang::GlobalDecl destructorGlobalDecl(destructor, clang::Dtor_Complete);
      auto *destructorFnAddr =
          cast<llvm::Function>(IGF.IGM.getAddrOfClangGlobalDecl(
              destructorGlobalDecl, NotForDefinition));

      SmallVector<llvm::Value *, 2> args;
      auto *thisArg = address.getAddress();
      if (IGF.IGM.getLLVMContext().supportsTypedPointers())
        thisArg = IGF.coerceValue(address.getAddress(),
                                  destructorFnAddr->getArg(0)->getType(),
                                  IGF.IGM.DataLayout);
      args.push_back(thisArg);
      llvm::Value *implicitParam =
          clang::CodeGen::getCXXDestructorImplicitParam(
              IGF.IGM.getClangCGM(), IGF.Builder.GetInsertBlock(),
              IGF.Builder.GetInsertPoint(), destructor, clang::Dtor_Complete,
              false, false);
      if (implicitParam) {
        implicitParam = IGF.coerceValue(implicitParam,
                                        destructorFnAddr->getArg(1)->getType(),
                                        IGF.IGM.DataLayout);
        args.push_back(implicitParam);
      }
      bool canThrow = false;
      if (IGF.IGM.isForeignExceptionHandlingEnabled()) {
        if (!IGF.IGM.isCxxNoThrow(destructor, /*defaultNoThrow=*/true))
          canThrow = true;
      }
      if (canThrow) {
        IGF.createExceptionTrapScope([&](llvm::BasicBlock *invokeNormalDest,
                                         llvm::BasicBlock *invokeUnwindDest) {
          IGF.Builder.createInvoke(destructorFnAddr->getFunctionType(),
                                   destructorFnAddr, args, invokeNormalDest,
                                   invokeUnwindDest);
        });
        return;
      }

      IGF.Builder.CreateCall(destructorFnAddr->getFunctionType(),
                             destructorFnAddr, args);
    }

    TypeLayoutEntry
    *buildTypeLayoutEntry(IRGenModule &IGM,
                          SILType T,
                          bool useStructLayouts) const override {
      if (!useStructLayouts || getCXXDestructor(T) ||
          !areFieldsABIAccessible()) {
        return IGM.typeLayoutCache.getOrCreateTypeInfoBasedEntry(*this, T);
      }

      std::vector<TypeLayoutEntry *> fields;
      for (auto &field : getFields()) {
        auto fieldTy = field.getType(IGM, T);
        if (!fieldTy) {
          return IGM.typeLayoutCache.getOrCreateTypeInfoBasedEntry(*this, T);
        }
        fields.push_back(
            field.getTypeInfo().buildTypeLayoutEntry(IGM, fieldTy, useStructLayouts));
      }
      assert(!fields.empty() &&
             "Empty structs should not be AddressOnlyRecordTypeInfo");

      if (fields.size() == 1 && getBestKnownAlignment() == *fields[0]->fixedAlignment(IGM)) {
        return fields[0];
      }

      return IGM.typeLayoutCache.getOrCreateAlignedGroupEntry(
          fields, T, getBestKnownAlignment().getValue(), *this);
    }

    void initializeFromParams(IRGenFunction &IGF, Explosion &params,
                              Address addr, SILType T,
                              bool isOutlined) const override {
      llvm_unreachable("Address-only C++ types must be created by C++ special "
                       "member functions.");
    }

    void initializeWithCopy(IRGenFunction &IGF, Address destAddr,
                            Address srcAddr, SILType T,
                            bool isOutlined) const override {
      if (auto copyConstructor = findCopyConstructor()) {
        emitCopyWithCopyConstructor(IGF, T, copyConstructor,
                                    srcAddr.getAddress(),
                                    destAddr.getAddress());
        return;
      }
      StructTypeInfoBase<AddressOnlyCXXClangRecordTypeInfo, FixedTypeInfo,
                         ClangFieldInfo>::initializeWithCopy(IGF, destAddr,
                                                             srcAddr, T,
                                                             isOutlined);
    }

    void assignWithCopy(IRGenFunction &IGF, Address destAddr, Address srcAddr,
                        SILType T, bool isOutlined) const override {
      if (auto copyConstructor = findCopyConstructor()) {
        destroy(IGF, destAddr, T, isOutlined);
        emitCopyWithCopyConstructor(IGF, T, copyConstructor,
                                    srcAddr.getAddress(),
                                    destAddr.getAddress());
        return;
      }
      StructTypeInfoBase<AddressOnlyCXXClangRecordTypeInfo, FixedTypeInfo,
                         ClangFieldInfo>::assignWithCopy(IGF, destAddr, srcAddr,
                                                         T, isOutlined);
    }

    void initializeWithTake(IRGenFunction &IGF, Address dest, Address src,
                            SILType T, bool isOutlined) const override {
      if (auto moveConstructor = findMoveConstructor()) {
        emitCopyWithCopyConstructor(IGF, T, moveConstructor,
                                    src.getAddress(),
                                    dest.getAddress());
        destroy(IGF, src, T, isOutlined);
        return;
      }

      if (auto copyConstructor = findCopyConstructor()) {
        emitCopyWithCopyConstructor(IGF, T, copyConstructor,
                                    src.getAddress(),
                                    dest.getAddress());
        destroy(IGF, src, T, isOutlined);
        return;
      }

      StructTypeInfoBase<AddressOnlyCXXClangRecordTypeInfo, FixedTypeInfo,
                         ClangFieldInfo>::initializeWithTake(IGF, dest, src, T,
                                                             isOutlined);
    }

    void assignWithTake(IRGenFunction &IGF, Address dest, Address src, SILType T,
                        bool isOutlined) const override {
      if (auto moveConstructor = findMoveConstructor()) {
        destroy(IGF, dest, T, isOutlined);
        emitCopyWithCopyConstructor(IGF, T, moveConstructor,
                                    src.getAddress(),
                                    dest.getAddress());
        destroy(IGF, src, T, isOutlined);
        return;
      }

      if (auto copyConstructor = findCopyConstructor()) {
        destroy(IGF, dest, T, isOutlined);
        emitCopyWithCopyConstructor(IGF, T, copyConstructor,
                                    src.getAddress(),
                                    dest.getAddress());
        destroy(IGF, src, T, isOutlined);
        return;
      }

      StructTypeInfoBase<AddressOnlyCXXClangRecordTypeInfo, FixedTypeInfo,
                         ClangFieldInfo>::assignWithTake(IGF, dest, src, T,
                                                         isOutlined);
    }

    llvm::NoneType getNonFixedOffsets(IRGenFunction &IGF) const {
      return llvm::None;
    }
    llvm::NoneType getNonFixedOffsets(IRGenFunction &IGF, SILType T) const {
      return llvm::None;
    }
    MemberAccessStrategy
    getNonFixedFieldAccessStrategy(IRGenModule &IGM, SILType T,
                                   const ClangFieldInfo &field) const {
      llvm_unreachable("non-fixed field in Clang type?");
    }
  };

  /// A type implementation for loadable struct types.
  class LoadableStructTypeInfo final
      : public StructTypeInfoBase<LoadableStructTypeInfo, LoadableTypeInfo> {
    using super = StructTypeInfoBase<LoadableStructTypeInfo, LoadableTypeInfo>;
  public:
    LoadableStructTypeInfo(ArrayRef<StructFieldInfo> fields,
                           unsigned explosionSize,
                           llvm::Type *storageType, Size size,
                           SpareBitVector &&spareBits,
                           Alignment align,
                           IsTriviallyDestroyable_t isTriviallyDestroyable,
                           IsCopyable_t isCopyable,
                           IsFixedSize_t alwaysFixedSize)
      : StructTypeInfoBase(StructTypeInfoKind::LoadableStructTypeInfo,
                           fields, explosionSize,
                           storageType, size, std::move(spareBits),
                           align, isTriviallyDestroyable,
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

      if (!areFieldsABIAccessible()) {
        return IGM.typeLayoutCache.getOrCreateResilientEntry(T);
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

      if (fields.size() == 1 && isFixedSize() &&
          getBestKnownAlignment() == *fields[0]->fixedAlignment(IGM)) {
        return fields[0];
      }

      return IGM.typeLayoutCache.getOrCreateAlignedGroupEntry(
          fields, T, getBestKnownAlignment().getValue(), *this);
    }

    void initializeFromParams(IRGenFunction &IGF, Explosion &params,
                              Address addr, SILType T,
                              bool isOutlined) const override {
      LoadableStructTypeInfo::initialize(IGF, params, addr, isOutlined);
    }
    llvm::NoneType getNonFixedOffsets(IRGenFunction &IGF) const {
      return llvm::None;
    }
    llvm::NoneType getNonFixedOffsets(IRGenFunction &IGF, SILType T) const {
      return llvm::None;
    }
    MemberAccessStrategy
    getNonFixedFieldAccessStrategy(IRGenModule &IGM, SILType T,
                                   const StructFieldInfo &field) const {
      llvm_unreachable("non-fixed field in loadable type?");
    }
    
    void consume(IRGenFunction &IGF, Explosion &explosion,
                 Atomicity atomicity, SILType T) const override {
      // If the struct has a deinit declared, then call it to consume the
      // value.
      if (tryEmitConsumeUsingDeinit(IGF, explosion, T)) {
        return;
      }
      
      // Otherwise, do elementwise destruction of the value.
      return super::consume(IGF, explosion, atomicity, T);
    }
  };

  /// A type implementation for non-loadable but fixed-size struct types.
  class FixedStructTypeInfo final
      : public StructTypeInfoBase<FixedStructTypeInfo,
                                  IndirectTypeInfo<FixedStructTypeInfo,
                                                   FixedTypeInfo>> {
  public:
    // FIXME: Spare bits between struct members.
    FixedStructTypeInfo(ArrayRef<StructFieldInfo> fields, llvm::Type *T,
                        Size size, SpareBitVector &&spareBits,
                        Alignment align,
                        IsTriviallyDestroyable_t isTriviallyDestroyable,
                        IsBitwiseTakable_t isBT,
                        IsCopyable_t isCopyable,
                        IsFixedSize_t alwaysFixedSize)
      : StructTypeInfoBase(StructTypeInfoKind::FixedStructTypeInfo,
                           fields, T, size, std::move(spareBits), align,
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

      if (!areFieldsABIAccessible()) {
        return IGM.typeLayoutCache.getOrCreateResilientEntry(T);
      }

      std::vector<TypeLayoutEntry *> fields;
      for (auto &field : getFields()) {
        auto fieldTy = field.getType(IGM, T);
        fields.push_back(
            field.getTypeInfo().buildTypeLayoutEntry(IGM, fieldTy, useStructLayouts));
      }
      assert(!fields.empty() &&
             "Empty structs should not be FixedStructTypeInfo");

      // if (fields.size() == 1  && getBestKnownAlignment() == *fields[0]->fixedAlignment(IGM)) {
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
    MemberAccessStrategy
    getNonFixedFieldAccessStrategy(IRGenModule &IGM, SILType T,
                                   const StructFieldInfo &field) const {
      llvm_unreachable("non-fixed field in fixed struct?");
    }
  };

  /// Accessor for the non-fixed offsets of a struct type.
  class StructNonFixedOffsets : public NonFixedOffsetsImpl {
    SILType TheStruct;
  public:
    StructNonFixedOffsets(SILType type) : TheStruct(type) {
      assert(TheStruct.getStructOrBoundGenericStruct());
    }

    llvm::Value *getOffsetForIndex(IRGenFunction &IGF, unsigned index) override {
      auto &layout =
          IGF.IGM.getMetadataLayout(TheStruct.getStructOrBoundGenericStruct());
      auto offset = layout.getFieldOffset(
          IGF, layout.getDecl()->getStoredProperties()[index]);
      llvm::Value *metadata = IGF.emitTypeMetadataRefForLayout(TheStruct);
      auto field = IGF.emitAddressAtOffset(metadata, offset, IGF.IGM.Int32Ty,
                                           IGF.IGM.getPointerAlignment());
      return IGF.Builder.CreateLoad(field);
    }

    MemberAccessStrategy getFieldAccessStrategy(IRGenModule &IGM,
                                                unsigned nonFixedIndex) {
      auto start =
        IGM.getMetadataLayout(TheStruct.getStructOrBoundGenericStruct())
          .getFieldOffsetVectorOffset();

      // FIXME: Handle resilience
      auto indirectOffset = start.getStatic() +
        (IGM.getPointerSize() * nonFixedIndex);

      return MemberAccessStrategy::getIndirectFixed(indirectOffset,
                               MemberAccessStrategy::OffsetKind::Bytes_Word);
    }
  };

  /// A type implementation for non-fixed struct types.
  class NonFixedStructTypeInfo final
      : public StructTypeInfoBase<NonFixedStructTypeInfo,
                                  WitnessSizedTypeInfo<NonFixedStructTypeInfo>>
  {
  public:
    NonFixedStructTypeInfo(ArrayRef<StructFieldInfo> fields,
                           FieldsAreABIAccessible_t fieldsAccessible,
                           llvm::Type *T,
                           Alignment align,
                           IsTriviallyDestroyable_t isTriviallyDestroyable,
                           IsBitwiseTakable_t isBT,
                           IsCopyable_t isCopyable,
                           IsABIAccessible_t structAccessible)
      : StructTypeInfoBase(StructTypeInfoKind::NonFixedStructTypeInfo,
                           fields, fieldsAccessible,
                           T, align, isTriviallyDestroyable, isBT, isCopyable,
                           structAccessible) {
    }

    TypeLayoutEntry
    *buildTypeLayoutEntry(IRGenModule &IGM,
                          SILType T,
                          bool useStructLayouts) const override {
      if (!areFieldsABIAccessible()) {
        return IGM.typeLayoutCache.getOrCreateResilientEntry(T);
      }

      std::vector<TypeLayoutEntry *> fields;
      for (auto &field : getFields()) {
        auto fieldTy = field.getType(IGM, T);
        fields.push_back(
            field.getTypeInfo().buildTypeLayoutEntry(IGM, fieldTy, useStructLayouts));
      }
      assert(!fields.empty() &&
             "Empty structs should not be NonFixedStructTypeInfo");

      // if (fields.size() == 1 && getBestKnownAlignment() > Alignment(1)) {
      //   return fields[0];
      // }

      return IGM.typeLayoutCache.getOrCreateAlignedGroupEntry(
          fields, T, getBestKnownAlignment().getValue(), *this);
    }

    // We have an indirect schema.
    void getSchema(ExplosionSchema &s) const override {
      s.add(ExplosionSchema::Element::forAggregate(getStorageType(),
                                                   getBestKnownAlignment()));
    }

    StructNonFixedOffsets
    getNonFixedOffsets(IRGenFunction &IGF, SILType T) const {
      return StructNonFixedOffsets(T);
    }

    MemberAccessStrategy
    getNonFixedFieldAccessStrategy(IRGenModule &IGM, SILType T,
                                   const StructFieldInfo &field) const {
      return StructNonFixedOffsets(T).getFieldAccessStrategy(IGM,
                                              field.getNonFixedElementIndex());
    }

    llvm::Value *getEnumTagSinglePayload(IRGenFunction &IGF,
                                         llvm::Value *numEmptyCases,
                                         Address structAddr,
                                         SILType structType,
                                         bool isOutlined) const override {
      // If we're not emitting the value witness table's implementation,
      // just call that.
      if (!isOutlined) {
        return emitGetEnumTagSinglePayloadCall(IGF, structType, numEmptyCases,
                                               structAddr);
      }

      return emitGetEnumTagSinglePayloadGenericCall(IGF, structType, *this,
                                                    numEmptyCases, structAddr,
        [this,structType](IRGenFunction &IGF, Address structAddr,
                          llvm::Value *structNumXI) {
          return withExtraInhabitantProvidingField(IGF, structAddr, structType,
                                                   structNumXI, IGF.IGM.Int32Ty,
            [&](const FieldImpl &field, llvm::Value *numXI) -> llvm::Value* {
              Address fieldAddr = asImpl().projectFieldAddress(
                                           IGF, structAddr, structType, field);
              auto fieldTy = field.getType(IGF.IGM, structType);
              return field.getTypeInfo()
                          .getExtraInhabitantTagDynamic(IGF, fieldAddr, fieldTy,
                                                     numXI, /*outlined*/ false);
            });
        });
    }

    void storeEnumTagSinglePayload(IRGenFunction &IGF,
                                   llvm::Value *whichCase,
                                   llvm::Value *numEmptyCases,
                                   Address structAddr,
                                   SILType structType,
                                   bool isOutlined) const override {
      // If we're not emitting the value witness table's implementation,
      // just call that.
      if (!isOutlined) {
        return emitStoreEnumTagSinglePayloadCall(IGF, structType, whichCase,
                                                 numEmptyCases, structAddr);
      }

      emitStoreEnumTagSinglePayloadGenericCall(IGF, structType, *this,
                                               whichCase, numEmptyCases,
                                               structAddr,
        [this,structType](IRGenFunction &IGF, Address structAddr,
                          llvm::Value *tag, llvm::Value *structNumXI) {
          withExtraInhabitantProvidingField(IGF, structAddr, structType,
                                            structNumXI, IGF.IGM.VoidTy,
            [&](const FieldImpl &field, llvm::Value *numXI) -> llvm::Value* {
              Address fieldAddr = asImpl().projectFieldAddress(
                                           IGF, structAddr, structType, field);
              auto fieldTy = field.getType(IGF.IGM, structType);
              field.getTypeInfo()
                   .storeExtraInhabitantTagDynamic(IGF, tag, fieldAddr, fieldTy,
                                                   /*outlined*/ false);
              return nullptr;
            });
        });
    }
  };

  class StructTypeBuilder :
    public RecordTypeBuilder<StructTypeBuilder, StructFieldInfo, VarDecl*> {

    llvm::StructType *StructTy;
    CanType TheStruct;
  public:
    StructTypeBuilder(IRGenModule &IGM, llvm::StructType *structTy,
                      CanType type) :
      RecordTypeBuilder(IGM), StructTy(structTy), TheStruct(type) {
    }

    LoadableStructTypeInfo *createLoadable(ArrayRef<StructFieldInfo> fields,
                                           StructLayout &&layout,
                                           unsigned explosionSize) {
      return LoadableStructTypeInfo::create(fields,
                                            explosionSize,
                                            layout.getType(),
                                            layout.getSize(),
                                            std::move(layout.getSpareBits()),
                                            layout.getAlignment(),
                                            layout.isTriviallyDestroyable(),
                                            layout.isCopyable(),
                                            layout.isAlwaysFixedSize());
    }

    FixedStructTypeInfo *createFixed(ArrayRef<StructFieldInfo> fields,
                                     StructLayout &&layout) {
      return FixedStructTypeInfo::create(fields, layout.getType(),
                                         layout.getSize(),
                                         std::move(layout.getSpareBits()),
                                         layout.getAlignment(),
                                         layout.isTriviallyDestroyable(),
                                         layout.isBitwiseTakable(),
                                         layout.isCopyable(),
                                         layout.isAlwaysFixedSize());
    }

    NonFixedStructTypeInfo *createNonFixed(ArrayRef<StructFieldInfo> fields,
                                     FieldsAreABIAccessible_t fieldsAccessible,
                                           StructLayout &&layout) {
      auto structAccessible = IsABIAccessible_t(
        IGM.getSILModule().isTypeMetadataAccessible(TheStruct));
      return NonFixedStructTypeInfo::create(fields, fieldsAccessible,
                                            layout.getType(),
                                            layout.getAlignment(),
                                            layout.isTriviallyDestroyable(),
                                            layout.isBitwiseTakable(),
                                            layout.isCopyable(),
                                            structAccessible);
    }

    StructFieldInfo getFieldInfo(unsigned index,
                                 VarDecl *field, const TypeInfo &fieldTI) {
      return StructFieldInfo(field, fieldTI);
    }

    SILType getType(VarDecl *field) {
      assert(field->getDeclContext() == TheStruct->getAnyNominal());
      auto silType = SILType::getPrimitiveAddressType(TheStruct);
      return silType.getFieldType(
          field, IGM.getSILModule(),
          IGM.getMaximalTypeExpansionContext());
    }

    StructLayout performLayout(ArrayRef<const TypeInfo *> fieldTypes) {
      return StructLayout(IGM, TheStruct->getAnyNominal(),
                          LayoutKind::NonHeapObject,
                          LayoutStrategy::Optimal, fieldTypes, StructTy);
    }
  };

/// A class for lowering Clang records.
class ClangRecordLowering {
  IRGenModule &IGM;
  StructDecl *SwiftDecl;
  SILType SwiftType;
  const clang::RecordDecl *ClangDecl;
  const clang::ASTContext &ClangContext;
  const clang::ASTRecordLayout &ClangLayout;
  const Size TotalStride;
  const Alignment TotalAlignment;
  SpareBitVector SpareBits;

  SmallVector<llvm::Type *, 8> LLVMFields;
  SmallVector<ClangFieldInfo, 8> FieldInfos;
  Size NextOffset = Size(0);
  unsigned NextExplosionIndex = 0;
public:
  ClangRecordLowering(IRGenModule &IGM, StructDecl *swiftDecl,
                      const clang::RecordDecl *clangDecl,
                      SILType swiftType)
    : IGM(IGM), SwiftDecl(swiftDecl), SwiftType(swiftType),
      ClangDecl(clangDecl), ClangContext(clangDecl->getASTContext()),
      ClangLayout(ClangContext.getASTRecordLayout(clangDecl)),
      TotalStride(Size(ClangLayout.getSize().getQuantity())),
      TotalAlignment(IGM.getCappedAlignment(
                                       Alignment(ClangLayout.getAlignment()))) {
  }

  void collectRecordFields() {
    if (ClangDecl->isUnion()) {
      collectUnionFields();
    } else {
      collectBases();
      collectStructFields();
    }
  }

  const TypeInfo *createTypeInfo(llvm::StructType *llvmType) {
    llvmType->setBody(LLVMFields, /*packed*/ true);
    if (SwiftType.getStructOrBoundGenericStruct()->isCxxNonTrivial()) {
      return AddressOnlyCXXClangRecordTypeInfo::create(
          FieldInfos, llvmType, TotalStride, TotalAlignment, ClangDecl);
    }
    if (SwiftType.getStructOrBoundGenericStruct()->isNonTrivialPtrAuth()) {
      return AddressOnlyPointerAuthRecordTypeInfo::create(
          FieldInfos, llvmType, TotalStride, TotalAlignment, ClangDecl);
    }
    return LoadableClangRecordTypeInfo::create(
        FieldInfos, NextExplosionIndex, llvmType, TotalStride,
        std::move(SpareBits), TotalAlignment, ClangDecl);
  }

private:
  /// Collect all the fields of a union.
  void collectUnionFields() {
    addOpaqueField(Size(0), TotalStride);
  }

  static bool isImportOfClangField(VarDecl *swiftField,
                                   const clang::FieldDecl *clangField) {
    assert(swiftField->hasClangNode());
    return (swiftField->getClangNode().castAsDecl() == clangField);
  }

  void collectBases() {
    auto &layout = ClangDecl->getASTContext().getASTRecordLayout(ClangDecl);

    if (auto cxxRecord = dyn_cast<clang::CXXRecordDecl>(ClangDecl)) {
      for (auto base : cxxRecord->bases()) {
        if (base.isVirtual())
          continue;

        auto baseType = base.getType().getCanonicalType();

        auto baseRecord = cast<clang::RecordType>(baseType)->getDecl();
        auto baseCxxRecord = cast<clang::CXXRecordDecl>(baseRecord);

        if (baseCxxRecord->isEmpty())
          continue;

        auto offset = layout.getBaseClassOffset(baseCxxRecord);
        auto size = ClangDecl->getASTContext().getTypeSizeInChars(baseType);
        addOpaqueField(Size(offset.getQuantity()), Size(size.getQuantity()));
      }
    }
  }

  void collectStructFields() {
    auto cfi = ClangDecl->field_begin(), cfe = ClangDecl->field_end();
    auto swiftProperties = SwiftDecl->getStoredProperties();
    auto sfi = swiftProperties.begin(), sfe = swiftProperties.end();

    while (cfi != cfe) {
      const clang::FieldDecl *clangField = *cfi++;

      // Bitfields are currently never mapped, but that doesn't mean
      // we don't have to copy them.
      if (clangField->isBitField()) {
        // Collect all of the following bitfields.
        unsigned bitStart =
          ClangLayout.getFieldOffset(clangField->getFieldIndex());
        unsigned bitEnd = bitStart + clangField->getBitWidthValue(ClangContext);

        while (cfi != cfe && (*cfi)->isBitField()) {
          clangField = *cfi++;
          unsigned nextStart =
            ClangLayout.getFieldOffset(clangField->getFieldIndex());
          assert(nextStart >= bitEnd && "laying out bit-fields out of order?");

          // In a heuristic effort to reduce the number of weird-sized
          // fields, whenever we see a bitfield starting on a 32-bit
          // boundary, start a new storage unit.
          if (nextStart % 32 == 0) {
            addOpaqueBitField(bitStart, bitEnd);
            bitStart = nextStart;
          }

          bitEnd = nextStart + clangField->getBitWidthValue(ClangContext);
        }

        addOpaqueBitField(bitStart, bitEnd);
        continue;
      }

      VarDecl *swiftField;
      if (sfi != sfe) {
        swiftField = *sfi;
        if (isImportOfClangField(swiftField, clangField)) {
          ++sfi;
        } else {
          swiftField = nullptr;
        }
      } else {
        swiftField = nullptr;
      }

      // Try to position this field.  If this fails, it's because we
      // didn't lay out padding correctly.
      addStructField(clangField, swiftField);
    }

    assert(sfi == sfe && "more Swift fields than there were Clang fields?");

    // We never take advantage of tail padding, because that would prevent
    // us from passing the address of the object off to C, which is a pretty
    // likely scenario for imported C types.
    assert(NextOffset <= TotalStride);
    assert(SpareBits.size() <= TotalStride.getValueInBits());
    if (NextOffset < TotalStride) {
      addPaddingField(TotalStride);
    }
  }

  /// Place the next struct field at its appropriate offset.
  void addStructField(const clang::FieldDecl *clangField,
                      VarDecl *swiftField) {
    unsigned fieldOffset = ClangLayout.getFieldOffset(clangField->getFieldIndex());
    assert(!clangField->isBitField());
    Size offset(fieldOffset / 8);

    // If we have a Swift import of this type, use our lowered information.
    if (swiftField) {
      auto &fieldTI = cast<FixedTypeInfo>(IGM.getTypeInfo(
          SwiftType.getFieldType(swiftField, IGM.getSILModule(),
                                 IGM.getMaximalTypeExpansionContext())));
      addField(swiftField, offset, fieldTI);
      return;
    }

    // Otherwise, add it as an opaque blob.
    auto fieldSize = ClangContext.getTypeSizeInChars(clangField->getType());
    return addOpaqueField(offset, Size(fieldSize.getQuantity()));
  }

  /// Add opaque storage for bitfields spanning the given range of bits.
  void addOpaqueBitField(unsigned bitBegin, unsigned bitEnd) {
    assert(bitBegin <= bitEnd);

    // No need to add storage for zero-width bitfields.
    if (bitBegin == bitEnd) return;

    // Round up to an even number of bytes.
    assert(bitBegin % 8 == 0);
    Size offset = Size(bitBegin / 8);
    Size byteLength = Size((bitEnd - bitBegin + 7) / 8);

    addOpaqueField(offset, byteLength);
  }

  /// Add opaque storage at the given offset.
  void addOpaqueField(Size offset, Size fieldSize) {
    // No need to add storage for zero-size fields (e.g. incomplete array
    // decls).
    if (fieldSize.isZero()) return;

    auto &opaqueTI = IGM.getOpaqueStorageTypeInfo(fieldSize, Alignment(1));
    addField(nullptr, offset, opaqueTI);
  }

  /// Add storage for an (optional) Swift field at the given offset.
  void addField(VarDecl *swiftField, Size offset,
                const FixedTypeInfo &fieldType) {
    assert(offset >= NextOffset && "adding fields out of order");

    // Add a padding field if required.
    if (offset != NextOffset)
      addPaddingField(offset);

    addFieldInfo(swiftField, fieldType);
  }

  /// Add information to track a value field at the current offset.
  void addFieldInfo(VarDecl *swiftField, const FixedTypeInfo &fieldType) {
    bool isLoadableField = isa<LoadableTypeInfo>(fieldType);
    unsigned explosionSize = 0;
    if (isLoadableField)
      explosionSize = cast<LoadableTypeInfo>(fieldType).getExplosionSize();
    unsigned explosionBegin = NextExplosionIndex;
    NextExplosionIndex += explosionSize;
    unsigned explosionEnd = NextExplosionIndex;

    ElementLayout layout = ElementLayout::getIncomplete(fieldType);
    auto isEmpty = fieldType.isKnownEmpty(ResilienceExpansion::Maximal);
    if (isEmpty)
      layout.completeEmptyTailAllocatedCType(
          fieldType.isTriviallyDestroyable(ResilienceExpansion::Maximal), NextOffset);
    else
      layout.completeFixed(fieldType.isTriviallyDestroyable(ResilienceExpansion::Maximal),
                           NextOffset, LLVMFields.size());

    if (isLoadableField)
      FieldInfos.push_back(
          ClangFieldInfo(swiftField, layout, explosionBegin, explosionEnd));
    else
      FieldInfos.push_back(ClangFieldInfo(swiftField, layout, fieldType));

    if (!isEmpty) {
      LLVMFields.push_back(fieldType.getStorageType());
      NextOffset += fieldType.getFixedSize();
      SpareBits.append(fieldType.getSpareBits());
    }
  }

  /// Add padding to get up to the given offset.
  void addPaddingField(Size offset) {
    assert(offset > NextOffset);
    Size count = offset - NextOffset;
    LLVMFields.push_back(llvm::ArrayType::get(IGM.Int8Ty, count.getValue()));
    NextOffset = offset;
    SpareBits.appendSetBits(count.getValueInBits());
  }
};

} // end anonymous namespace

/// A convenient macro for delegating an operation to all of the
/// various struct implementations.
#define FOR_STRUCT_IMPL(IGF, type, op, ...)                                    \
  do {                                                                         \
    auto &structTI = IGF.getTypeInfo(type);                                    \
    switch (getStructTypeInfoKind(structTI)) {                                 \
    case StructTypeInfoKind::LoadableClangRecordTypeInfo:                      \
      return structTI.as<LoadableClangRecordTypeInfo>().op(IGF, __VA_ARGS__);  \
    case StructTypeInfoKind::AddressOnlyClangRecordTypeInfo:                   \
      return structTI.as<AddressOnlyCXXClangRecordTypeInfo>().op(IGF,          \
                                                                 __VA_ARGS__); \
    case StructTypeInfoKind::LoadableStructTypeInfo:                           \
      return structTI.as<LoadableStructTypeInfo>().op(IGF, __VA_ARGS__);       \
    case StructTypeInfoKind::FixedStructTypeInfo:                              \
      return structTI.as<FixedStructTypeInfo>().op(IGF, __VA_ARGS__);          \
    case StructTypeInfoKind::NonFixedStructTypeInfo:                           \
      return structTI.as<NonFixedStructTypeInfo>().op(IGF, __VA_ARGS__);       \
    case StructTypeInfoKind::ResilientStructTypeInfo:                          \
      llvm_unreachable("resilient structs are opaque");                        \
    }                                                                          \
    llvm_unreachable("bad struct type info kind!");                            \
  } while (0)

Address irgen::projectPhysicalStructMemberAddress(IRGenFunction &IGF,
                                                  Address base,
                                                  SILType baseType,
                                                  VarDecl *field) {
  FOR_STRUCT_IMPL(IGF, baseType, projectFieldAddress, base,
                  baseType, field);
}

void irgen::projectPhysicalStructMemberFromExplosion(IRGenFunction &IGF,
                                                     SILType baseType,
                                                     Explosion &base,
                                                     VarDecl *field,
                                                     Explosion &out) {
  FOR_STRUCT_IMPL(IGF, baseType, projectFieldFromExplosion, base, field, out);
}

llvm::Constant *irgen::emitPhysicalStructMemberFixedOffset(IRGenModule &IGM,
                                                           SILType baseType,
                                                           VarDecl *field) {
  FOR_STRUCT_IMPL(IGM, baseType, getConstantFieldOffset, field);
}

MemberAccessStrategy
irgen::getPhysicalStructMemberAccessStrategy(IRGenModule &IGM,
                                             SILType baseType, VarDecl *field) {
  FOR_STRUCT_IMPL(IGM, baseType, getFieldAccessStrategy, baseType, field);
}

llvm::Optional<unsigned> irgen::getPhysicalStructFieldIndex(IRGenModule &IGM,
                                                            SILType baseType,
                                                            VarDecl *field) {
  FOR_STRUCT_IMPL(IGM, baseType, getFieldIndexIfNotEmpty, field);
}

const TypeInfo *irgen::getPhysicalStructFieldTypeInfo(IRGenModule &IGM,
                                                      SILType baseType,
                                                      VarDecl *field) {
  FOR_STRUCT_IMPL(IGM, baseType, getFieldTypeInfo, field);
}

void IRGenModule::emitStructDecl(StructDecl *st) {
  if (!IRGen.hasLazyMetadata(st)) {
    emitStructMetadata(*this, st);
    emitFieldDescriptor(st);
  }

  emitNestedTypeDecls(st->getMembers());
}

void IRGenModule::maybeEmitOpaqueTypeDecl(OpaqueTypeDecl *opaque) {
  if (Lowering::shouldSkipLowering(opaque))
    return;

  if (IRGen.Opts.EnableAnonymousContextMangledNames) {
    // If we're emitting anonymous context mangled names for debuggability,
    // then emit all opaque type descriptors and make them runtime-discoverable
    // so that remote ast/mirror can recover them.
    addRuntimeResolvableType(opaque);
    if (IRGen.hasLazyMetadata(opaque))
      IRGen.noteUseOfOpaqueTypeDescriptor(opaque);
    else {
      if (IRGen.EmittedNonLazyOpaqueTypeDecls.insert(opaque).second)
        emitOpaqueTypeDecl(opaque);
    }
  } else if (!IRGen.hasLazyMetadata(opaque)) {
    if (IRGen.EmittedNonLazyOpaqueTypeDecls.insert(opaque).second)
      emitOpaqueTypeDecl(opaque);
  }
}

namespace {
  /// A type implementation for resilient struct types. This is not a
  /// StructTypeInfoBase at all, since we don't know anything about
  /// the struct's fields.
  class ResilientStructTypeInfo
      : public ResilientTypeInfo<ResilientStructTypeInfo>
  {
  public:
    ResilientStructTypeInfo(llvm::Type *T,
                            IsCopyable_t copyable,
                            IsABIAccessible_t abiAccessible)
      : ResilientTypeInfo(T, copyable, abiAccessible) {
      setSubclassKind((unsigned) StructTypeInfoKind::ResilientStructTypeInfo);
    }

    TypeLayoutEntry
    *buildTypeLayoutEntry(IRGenModule &IGM,
                          SILType T,
                          bool useStructLayouts) const override {
      return IGM.typeLayoutCache.getOrCreateResilientEntry(T);
    }
  };
} // end anonymous namespace

const TypeInfo *
TypeConverter::convertResilientStruct(IsCopyable_t copyable,
                                      IsABIAccessible_t abiAccessible) {
  llvm::Type *storageType = IGM.OpaqueTy;
  return new ResilientStructTypeInfo(storageType, copyable, abiAccessible);
}

const TypeInfo *TypeConverter::convertStructType(TypeBase *key, CanType type,
                                                 StructDecl *D){
  // All resilient structs have the same opaque lowering, since they are
  // indistinguishable as values --- except that we have to track
  // ABI-accessibility.
  //
  // Treat infinitely-sized types as resilient as well, since they can never
  // be concretized.
  if (IGM.isResilient(D, ResilienceExpansion::Maximal)
      || IGM.getSILTypes().getTypeLowering(SILType::getPrimitiveAddressType(type),
                                            TypeExpansionContext::minimal())
            .getRecursiveProperties().isInfinite()) {
    auto copyable = D->isMoveOnly()
      ? IsNotCopyable : IsCopyable;
    auto structAccessible =
      IsABIAccessible_t(IGM.getSILModule().isTypeMetadataAccessible(type));
    return &getResilientStructTypeInfo(copyable, structAccessible);
  }

  // Create the struct type.
  auto ty = IGM.createNominalType(type);

  // Register a forward declaration before we look at any of the child types.
  addForwardDecl(key);

  // Use different rules for types imported from C.
  if (D->hasClangNode()) {
    const clang::Decl *clangDecl = D->getClangNode().getAsDecl();
    assert(clangDecl && "Swift struct from an imported C macro?");

    if (auto clangRecord = dyn_cast<clang::RecordDecl>(clangDecl)) {
      ClangRecordLowering lowering(IGM, D, clangRecord,
                                   SILType::getPrimitiveObjectType(type));
      lowering.collectRecordFields();
      return lowering.createTypeInfo(ty);

    } else if (isa<clang::EnumDecl>(clangDecl)) {
      // Fall back to Swift lowering for the enum's representation as a struct.
      assert(D->getStoredProperties().size() == 1 &&
             "Struct representation of a Clang enum should wrap one value");
    } else if (clangDecl->hasAttr<clang::SwiftNewTypeAttr>()) {
      // Fall back to Swift lowering for the underlying type's
      // representation as a struct member.
      assert(D->getStoredProperties().size() == 1 &&
             "Struct representation of a swift_newtype should wrap one value");
    } else {
      llvm_unreachable("Swift struct represents unexpected imported type");
    }
  }

  // Collect all the fields from the type.
  SmallVector<VarDecl*, 8> fields;
  for (VarDecl *VD : D->getStoredProperties())
    fields.push_back(VD);

  // Build the type.
  StructTypeBuilder builder(IGM, ty, type);
  return builder.layout(fields);
}

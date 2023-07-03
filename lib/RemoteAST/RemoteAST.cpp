//===--- RemoteAST.cpp ----------------------------------------------------===//
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
// This file implements the RemoteAST interface.
//
//===----------------------------------------------------------------------===//

#include "swift/RemoteAST/RemoteAST.h"
#include "swift/Remote/MetadataReader.h"
#include "swift/Strings.h"
#include "swift/Subsystems.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTDemangler.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/TypeRepr.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Mangler.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/Demangling/Demangler.h"
#include "llvm/ADT/StringSwitch.h"

// TODO: Develop a proper interface for this.
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/SILOptions.h"
#include "swift/SIL/SILModule.h"
#include "../IRGen/IRGenModule.h"
#include "../IRGen/FixedTypeInfo.h"
#include "../IRGen/GenClass.h"
#include "../IRGen/GenStruct.h"
#include "../IRGen/GenTuple.h"
#include "../IRGen/MemberAccessStrategy.h"

using namespace swift;
using namespace swift::remote;
using namespace swift::remoteAST;

using irgen::Alignment;
using irgen::Size;

static inline RemoteAddress operator+(RemoteAddress address, Size offset) {
  return RemoteAddress(address.getAddressData() + offset.getValue());
}

namespace {

/// A "minimal" class for querying IRGen.
struct IRGenContext {
  const IRGenOptions IROpts;
  SILOptions SILOpts;
  Lowering::TypeConverter TC;
  std::unique_ptr<SILModule> SILMod;
  irgen::IRGenerator IRGen;
  irgen::IRGenModule IGM;

private:
  IRGenContext(ASTContext &ctx, ModuleDecl *module)
    : IROpts(createIRGenOptions()),
      TC(*module),
      SILMod(SILModule::createEmptyModule(module, TC, SILOpts)),
      IRGen(IROpts, *SILMod),
      IGM(IRGen, IRGen.createTargetMachine()) {}

  static IRGenOptions createIRGenOptions() {
    IRGenOptions IROpts;
    return IROpts;
  }

public:
  static std::unique_ptr<IRGenContext>
  create(ASTContext &ctx, DeclContext *nominalDC) {
    auto module = nominalDC->getParentModule();
    return std::unique_ptr<IRGenContext>(new IRGenContext(ctx, module));
  }
};

/// The basic implementation of the RemoteASTContext interface.
/// The template subclasses do target-specific logic.
class RemoteASTContextImpl {
  std::unique_ptr<IRGenContext> IRGen;
  llvm::Optional<Failure> CurFailure;

public:
  RemoteASTContextImpl() = default;
  virtual ~RemoteASTContextImpl() = default;

  virtual Result<Type>
  getTypeForRemoteTypeMetadata(RemoteAddress metadata, bool skipArtificial) = 0;
  virtual Result<MetadataKind>
  getKindForRemoteTypeMetadata(RemoteAddress metadata) = 0;
  virtual Result<NominalTypeDecl*>
  getDeclForRemoteNominalTypeDescriptor(RemoteAddress descriptor) = 0;
  virtual Result<RemoteAddress>
  getHeapMetadataForObject(RemoteAddress object) = 0;
  virtual Result<OpenedExistential>
  getDynamicTypeAndAddressForError(RemoteAddress object) = 0;
  virtual Result<OpenedExistential>
  getDynamicTypeAndAddressForExistential(RemoteAddress object,
                                         Type staticType) = 0;
  virtual Result<Type>
  getUnderlyingTypeForOpaqueType(remote::RemoteAddress opaqueDescriptor,
                                 SubstitutionMap substitutions,
                                 unsigned ordinal) = 0;
  Result<uint64_t>
  getOffsetOfMember(Type type, RemoteAddress optMetadata, StringRef memberName){
    // Sanity check: obviously invalid arguments.
    if (!type || memberName.empty())
      return Result<uint64_t>::emplaceFailure(Failure::BadArgument);

    // Sanity check: if the caller gave us a dependent type, there's no way
    // we can handle that.
    if (type->hasTypeParameter() || type->hasArchetype())
      return Result<uint64_t>::emplaceFailure(Failure::DependentArgument);

    // Split into cases.
    if (auto typeDecl = type->getNominalOrBoundGenericNominal()) {
      return getOffsetOfField(type, typeDecl, optMetadata, memberName);
    } else if (auto tupleType = type->getAs<TupleType>()) {
      return getOffsetOfTupleElement(tupleType, optMetadata, memberName);
    } else {
      return Result<uint64_t>::emplaceFailure(Failure::TypeHasNoSuchMember,
                                              memberName.str());
    }
  }

protected:
  template <class T, class DefaultFailureKindTy, class... DefaultFailureArgTys>
  Result<T> getFailureAsResult(DefaultFailureKindTy defaultFailureKind,
                               DefaultFailureArgTys &&...defaultFailureArgs) {
    // If we already have a failure, use that.
    if (CurFailure) {
      Result<T> result = std::move(*CurFailure);
      CurFailure.reset();
      return result;
    }

    // Otherwise, use the default failure.
    return Result<T>::emplaceFailure(defaultFailureKind,
               std::forward<DefaultFailureArgTys>(defaultFailureArgs)...);
  }

  template <class T>
  Result<T> getFailure() {
    return getFailureAsResult<T>(Failure::Unknown);
  }

  template <class T, class KindTy, class... ArgTys>
  Result<T> fail(KindTy kind, ArgTys &&...args) {
    return Result<T>::emplaceFailure(kind, std::forward<ArgTys>(args)...);
  }

private:
  virtual ASTBuilder &getBuilder() = 0;
  virtual MemoryReader &getReader() = 0;
  virtual bool readWordOffset(RemoteAddress address, int64_t *offset) = 0;
  virtual std::unique_ptr<IRGenContext> createIRGenContext() = 0;
  virtual Result<uint64_t>
  getOffsetOfTupleElementFromMetadata(RemoteAddress metadata,
                                      unsigned elementIndex) = 0;
  virtual Result<uint64_t>
  getOffsetOfFieldFromMetadata(RemoteAddress metadata,
                               StringRef memberName) = 0;

  IRGenContext *getIRGen() {
    if (!IRGen) IRGen = createIRGenContext();
    return IRGen.get();
  }

  Result<uint64_t>
  getOffsetOfField(Type type, NominalTypeDecl *typeDecl,
                   RemoteAddress optMetadata, StringRef memberName) {
    if (!isa<StructDecl>(typeDecl) && !isa<ClassDecl>(typeDecl))
      return fail<uint64_t>(Failure::Unimplemented,
                            "access members of this kind of type");

    // Try to find the member.
    VarDecl *member = findField(typeDecl, memberName);

    // If we found a member, try to find its offset statically.
    if (member && member->hasStorage() && !typeDecl->isResilient()) {
      if (auto irgen = getIRGen()) {
        return getOffsetOfFieldFromIRGen(irgen->IGM, type, typeDecl,
                                          optMetadata, member);
      }
    }

    // Try searching the metadata for a member with the given name.
    if (optMetadata) {
      return getOffsetOfFieldFromMetadata(optMetadata, memberName);
    }

    // Okay, that's everything we know how to try.

    // Use a specialized diagnostic if we couldn't find any such member.
    if (!member) {
      return fail<uint64_t>(Failure::TypeHasNoSuchMember, memberName.str());
    }

    return fail<uint64_t>(Failure::Unknown);
  }

  /// Look for an instance property of the given nominal type that's
  /// known to be stored.
  VarDecl *findField(NominalTypeDecl *typeDecl, StringRef memberName) {
    for (auto field : typeDecl->getStoredProperties()) {
      if (field->getName().str() == memberName)
        return field;
    }
    return nullptr;
  }

  using MemberAccessStrategy = irgen::MemberAccessStrategy;

  Result<uint64_t>
  getOffsetOfFieldFromIRGen(irgen::IRGenModule &IGM, Type type,
                            NominalTypeDecl *typeDecl,
                            RemoteAddress optMetadata, VarDecl *member) {
    SILType loweredTy = IGM.getLoweredType(type);

    MemberAccessStrategy strategy =
      (isa<StructDecl>(typeDecl)
        ? getPhysicalStructMemberAccessStrategy(IGM, loweredTy, member)
        : getPhysicalClassMemberAccessStrategy(IGM, loweredTy, member));

    switch (strategy.getKind()) {
    case MemberAccessStrategy::Kind::Complex:
      return fail<uint64_t>(Failure::Unimplemented,
                            "access members with complex storage");

    case MemberAccessStrategy::Kind::DirectFixed:
      return uint64_t(strategy.getDirectOffset().getValue());

    case MemberAccessStrategy::Kind::DirectGlobal: {
      RemoteAddress directOffsetAddress =
        getReader().getSymbolAddress(strategy.getDirectGlobalSymbol());
      if (!directOffsetAddress)
        return getFailure<uint64_t>();

      return readDirectOffset(directOffsetAddress,
                              strategy.getDirectOffsetKind());
    }

    case MemberAccessStrategy::Kind::IndirectFixed: {
      // We can't apply indirect offsets without metadata.
      if (!optMetadata)
        return fail<uint64_t>(Failure::Unimplemented,
                              "access generically-offset members without "
                              "metadata");

      Size indirectOffset = strategy.getIndirectOffset();
      return readIndirectOffset(optMetadata, indirectOffset,
                                strategy.getDirectOffsetKind());
    }

    case MemberAccessStrategy::Kind::IndirectGlobal: {
      // We can't apply indirect offsets without metadata.
      if (!optMetadata)
        return fail<uint64_t>(Failure::Unimplemented,
                              "access generically-offset members without "
                              "metadata");

      RemoteAddress indirectOffsetAddress =
        getReader().getSymbolAddress(strategy.getIndirectGlobalSymbol());

      Size indirectOffset;
      if (!readOffset(indirectOffsetAddress,
                      strategy.getIndirectOffsetKind(),
                      indirectOffset))
        return getFailure<uint64_t>();

      return readIndirectOffset(optMetadata, indirectOffset,
                                strategy.getDirectOffsetKind());
    }
    }
    llvm_unreachable("bad member MemberAccessStrategy");
  }

  bool readOffset(RemoteAddress address,
                  MemberAccessStrategy::OffsetKind kind,
                  Size &offset) {
    switch (kind) {
    case MemberAccessStrategy::OffsetKind::Bytes_Word: {
      int64_t rawOffset;
      if (!readWordOffset(address, &rawOffset))
        return false;
      offset = Size(rawOffset);
      return true;
    }
    }
    llvm_unreachable("bad offset kind");
  }

  Result<uint64_t> readIndirectOffset(RemoteAddress metadata,
                                      Size indirectOffset,
                                      MemberAccessStrategy::OffsetKind kind) {
    RemoteAddress directOffsetAddress = metadata + indirectOffset;
    return readDirectOffset(directOffsetAddress, kind);
  }


  Result<uint64_t> readDirectOffset(RemoteAddress directOffsetAddress,
                                    MemberAccessStrategy::OffsetKind kind) {
    Size directOffset;
    if (!readOffset(directOffsetAddress, kind, directOffset))
      return getFailure<uint64_t>();

    return uint64_t(directOffset.getValue());
  }

  /// Read the
  Result<uint64_t>
  getOffsetOfTupleElement(TupleType *type, RemoteAddress optMetadata,
                          StringRef memberName) {
    // Check that the member "name" is a valid index into the tuple.
    unsigned targetIndex;
    if (memberName.getAsInteger(10, targetIndex) ||
        targetIndex >= type->getNumElements())
      return fail<uint64_t>(Failure::TypeHasNoSuchMember, memberName.str());

    // Fast path: element 0 is always at offset 0.
    if (targetIndex == 0)
      return uint64_t(0);

    // Create an IRGen instance.
    auto irgen = getIRGen();
    if (!irgen)
      return Result<uint64_t>::emplaceFailure(Failure::Unknown);
    auto &IGM = irgen->IGM;
    SILType loweredTy = IGM.getLoweredType(type);

    // Only the runtime metadata knows the offsets of resilient members.
    auto &typeInfo = IGM.getTypeInfo(loweredTy);
    if (!isa<irgen::FixedTypeInfo>(&typeInfo))
      return Result<uint64_t>::emplaceFailure(Failure::NotFixedLayout);

    // If the type has a statically fixed offset, return that.
    if (auto offset =
          irgen::getFixedTupleElementOffset(IGM, loweredTy, targetIndex))
      return offset->getValue();

    // If we have metadata, go load from that.
    if (optMetadata)
      return getOffsetOfTupleElementFromMetadata(optMetadata, targetIndex);

    // Okay, reproduce tuple layout.

    // Find the last element with a known offset.  Note that we don't
    // have to ask IRGen about element 0 because we know its size is zero.
    Size lastOffset = Size(0);
    unsigned lastIndex = targetIndex;
    for (--lastIndex; lastIndex != 0; --lastIndex) {
      if (auto offset =
            irgen::getFixedTupleElementOffset(IGM, loweredTy, lastIndex)) {
        lastOffset = *offset;
        break;
      }
    }

    // Okay, iteratively build up from there.
    for (; ; ++lastIndex) {
      // Try to get the size and alignment of this element.
      SILType eltTy = loweredTy.getTupleElementType(lastIndex);
      auto sizeAndAlignment = getTypeSizeAndAlignment(IGM, eltTy);
      if (!sizeAndAlignment) return getFailure<uint64_t>();

      // Round up to the alignment of the element.
      lastOffset = lastOffset.roundUpToAlignment(sizeAndAlignment->second);

      // If this is the target, we're done.
      if (lastIndex == targetIndex)
        return lastOffset.getValue();

      // Otherwise, skip forward by the size of the element.
      lastOffset += sizeAndAlignment->first;
    }

    llvm_unreachable("didn't reach target index");
  }

  /// Attempt to discover the size and alignment of the given type.
  llvm::Optional<std::pair<Size, Alignment>>
  getTypeSizeAndAlignment(irgen::IRGenModule &IGM, SILType eltTy) {
    auto &eltTI = IGM.getTypeInfo(eltTy);
    if (auto fixedTI = dyn_cast<irgen::FixedTypeInfo>(&eltTI)) {
      return std::make_pair(fixedTI->getFixedSize(),
                            fixedTI->getFixedAlignment());
    }

    // TODO: handle resilient types
    return llvm::None;
  }
};

/// A template for generating target-specific implementations of the
/// RemoteASTContext interface.
template <class Runtime>
class RemoteASTContextConcreteImpl final : public RemoteASTContextImpl {
  MetadataReader<Runtime, ASTBuilder> Reader;

  ASTBuilder &getBuilder() override {
    return Reader.Builder;
  }

  MemoryReader &getReader() override {
    return *Reader.Reader;
  }

  bool readWordOffset(RemoteAddress address, int64_t *extendedOffset) override {
    using unsigned_size_t = typename Runtime::StoredSize;
    using signed_size_t = typename std::make_signed<unsigned_size_t>::type;
    signed_size_t offset;
    if (!getReader().readInteger(address, &offset))
      return false;

    *extendedOffset = offset;
    return true;
  }

public:
  RemoteASTContextConcreteImpl(std::shared_ptr<MemoryReader> &&reader,
                               ASTContext &ctx)
    : Reader(std::move(reader), ctx, GenericSignature()) {}

  Result<Type> getTypeForRemoteTypeMetadata(RemoteAddress metadata,
                                            bool skipArtificial) override {
    if (auto result = Reader.readTypeFromMetadata(metadata.getAddressData(),
                                                  skipArtificial))
      return result;
    return getFailure<Type>();
  }

  Result<MetadataKind>
  getKindForRemoteTypeMetadata(RemoteAddress metadata) override {
    auto result = Reader.readKindFromMetadata(metadata.getAddressData());
    if (result)
      return *result;
    return getFailure<MetadataKind>();
  }

  Result<NominalTypeDecl*>
  getDeclForRemoteNominalTypeDescriptor(RemoteAddress descriptor) override {
    if (auto result =
          Reader.readNominalTypeFromDescriptor(descriptor.getAddressData()))
      return dyn_cast<NominalTypeDecl>((GenericTypeDecl *) result);
    return getFailure<NominalTypeDecl*>();
  }

  std::unique_ptr<IRGenContext> createIRGenContext() override {
    return IRGenContext::create(getBuilder().getASTContext(),
                                getBuilder().getNotionalDC());
  }

  Result<uint64_t>
  getOffsetOfTupleElementFromMetadata(RemoteAddress metadata,
                                      unsigned index) override {
    typename Runtime::StoredSize offset;
    if (Reader.readTupleElementOffset(metadata.getAddressData(),
                                      index, &offset))
      return uint64_t(offset);
    return getFailure<uint64_t>();
  }

  Result<uint64_t>
  getOffsetOfFieldFromMetadata(RemoteAddress metadata,
                               StringRef memberName) override {
    // TODO: this would be useful for resilience
    return fail<uint64_t>(Failure::Unimplemented,
                          "look up field offset by name");
  }

  Result<RemoteAddress>
  getHeapMetadataForObject(RemoteAddress object) override {
    auto result = Reader.readMetadataFromInstance(object.getAddressData());
    if (result) return RemoteAddress(*result);
    return getFailure<RemoteAddress>();
  }

  Result<OpenedExistential>
  getDynamicTypeAndAddressClassExistential(RemoteAddress object) {
    auto pointerval = Reader.readResolvedPointerValue(object.getAddressData());
    if (!pointerval)
      return getFailure<OpenedExistential>();
    auto result = Reader.readMetadataFromInstance(*pointerval);
    if (!result)
      return getFailure<OpenedExistential>();
    auto typeResult = Reader.readTypeFromMetadata(result.value());
    if (!typeResult)
      return getFailure<OpenedExistential>();
    return OpenedExistential(std::move(typeResult),
                             RemoteAddress(*pointerval));
  }

  Result<OpenedExistential>
  getDynamicTypeAndAddressErrorExistential(RemoteAddress object,
                                           bool dereference=true) {
    if (dereference) {
      auto pointerval = Reader.readResolvedPointerValue(object.getAddressData());
      if (!pointerval)
        return getFailure<OpenedExistential>();
      object = RemoteAddress(*pointerval);
    }

    auto result =
        Reader.readMetadataAndValueErrorExistential(object);
    if (!result)
      return getFailure<OpenedExistential>();

    auto typeResult =
        Reader.readTypeFromMetadata(result->MetadataAddress.getAddressData());
    if (!typeResult)
      return getFailure<OpenedExistential>();


    // When the existential wraps a class type, LLDB expects that the
    // address returned is the class instance itself and not the address
    // of the reference.
    auto payloadAddress = result->PayloadAddress;
    if (!result->IsBridgedError &&
        typeResult->getClassOrBoundGenericClass()) {
      auto pointerval = Reader.readResolvedPointerValue(
          payloadAddress.getAddressData());
      if (!pointerval)
        return getFailure<OpenedExistential>();

      payloadAddress = RemoteAddress(*pointerval);
    }

    return OpenedExistential(std::move(typeResult),
                             std::move(payloadAddress));
  }

  Result<OpenedExistential>
  getDynamicTypeAndAddressOpaqueExistential(RemoteAddress object) {
    auto result = Reader.readMetadataAndValueOpaqueExistential(object);
    if (!result)
      return getFailure<OpenedExistential>();

    auto typeResult =
        Reader.readTypeFromMetadata(result->MetadataAddress.getAddressData());
    if (!typeResult)
      return getFailure<OpenedExistential>();

    // When the existential wraps a class type, LLDB expects that the
    // address returned is the class instance itself and not the address
    // of the reference.
    auto payloadAddress = result->PayloadAddress;
    if (typeResult->getClassOrBoundGenericClass()) {
      auto pointerval = Reader.readResolvedPointerValue(
          payloadAddress.getAddressData());
      if (!pointerval)
        return getFailure<OpenedExistential>();

      payloadAddress = RemoteAddress(*pointerval);
    }

    return OpenedExistential(std::move(typeResult),
                             std::move(payloadAddress));
  }

  Result<OpenedExistential>
  getDynamicTypeAndAddressExistentialMetatype(RemoteAddress object) {
    // The value of the address is just the input address.
    // The type is obtained through the following sequence of steps:
    // 1) Loading a pointer from the input address
    // 2) Reading it as metadata and resolving the type
    // 3) Wrapping the resolved type in an existential metatype.
    auto pointerval = Reader.readResolvedPointerValue(object.getAddressData());
    if (!pointerval)
      return getFailure<OpenedExistential>();
    auto typeResult = Reader.readTypeFromMetadata(*pointerval);
    if (!typeResult)
      return getFailure<OpenedExistential>();
    auto wrappedType = ExistentialMetatypeType::get(typeResult);
    if (!wrappedType)
      return getFailure<OpenedExistential>();
    return OpenedExistential(std::move(wrappedType),
                             std::move(object));
  }

  /// Resolve the dynamic type and the value address of an error existential
  /// object, Unlike getDynamicTypeAndAddressForExistential(), this function
  /// takes the address of the instance and not the address of the reference.
  Result<OpenedExistential>
  getDynamicTypeAndAddressForError(RemoteAddress object) override {
    return getDynamicTypeAndAddressErrorExistential(object,
                                                    /*dereference=*/false);
  }

  /// Resolve the dynamic type and the value address of an existential,
  /// given its address and its static type. For class and error existentials,
  /// this API takes a pointer to the instance reference rather than the
  /// instance reference itself.
  Result<OpenedExistential>
  getDynamicTypeAndAddressForExistential(RemoteAddress object,
                                         Type staticType) override {
    // If this is not an existential, give up.
    if (!staticType->isAnyExistentialType())
      return getFailure<OpenedExistential>();

    // Handle the case where this is an ExistentialMetatype.
    if (!staticType->isExistentialType())
      return getDynamicTypeAndAddressExistentialMetatype(object);

    // This should be an existential type at this point.
    auto layout = staticType->getExistentialLayout();
    switch (layout.getKind()) {
    case ExistentialLayout::Kind::Class:
      return getDynamicTypeAndAddressClassExistential(object);
    case ExistentialLayout::Kind::Error:
      return getDynamicTypeAndAddressErrorExistential(object);
    case ExistentialLayout::Kind::Opaque:
      return getDynamicTypeAndAddressOpaqueExistential(object);
    }
    llvm_unreachable("invalid type kind");
  }
  
  Result<Type>
  getUnderlyingTypeForOpaqueType(remote::RemoteAddress opaqueDescriptor,
                                 SubstitutionMap substitutions,
                                 unsigned ordinal) override {
    auto underlyingType = Reader
                              .readUnderlyingTypeForOpaqueTypeDescriptor(
                                  opaqueDescriptor.getAddressData(), ordinal)
                              .getType();

    if (!underlyingType)
      return getFailure<Type>();
    
    return underlyingType.subst(substitutions);
  }
};

} // end anonymous namespace

static RemoteASTContextImpl *createImpl(ASTContext &ctx,
                                      std::shared_ptr<MemoryReader> &&reader) {
  auto &target = ctx.LangOpts.Target;
  assert(target.isArch32Bit() || target.isArch64Bit());
  bool objcInterop = ctx.LangOpts.EnableObjCInterop;

  if (target.isArch32Bit()) {
    if (objcInterop) {
      using Target = External<WithObjCInterop<RuntimeTarget<4>>>;
      return new RemoteASTContextConcreteImpl<Target>(std::move(reader), ctx);
    } else {
      using Target = External<NoObjCInterop<RuntimeTarget<4>>>;
      return new RemoteASTContextConcreteImpl<Target>(std::move(reader), ctx);
    }
  } else {
    if (objcInterop) {
      using Target = External<WithObjCInterop<RuntimeTarget<8>>>;
      return new RemoteASTContextConcreteImpl<Target>(std::move(reader), ctx);
    } else {
      using Target = External<NoObjCInterop<RuntimeTarget<8>>>;
      return new RemoteASTContextConcreteImpl<Target>(std::move(reader), ctx);
    }
  }
}

static RemoteASTContextImpl *asImpl(void *impl) {
  return static_cast<RemoteASTContextImpl*>(impl);
}

RemoteASTContext::RemoteASTContext(ASTContext &ctx,
                                   std::shared_ptr<MemoryReader> reader)
  : Impl(createImpl(ctx, std::move(reader))) {
}

RemoteASTContext::~RemoteASTContext() {
  delete asImpl(Impl);
}

Result<Type>
RemoteASTContext::getTypeForRemoteTypeMetadata(RemoteAddress address,
                                               bool skipArtificial) {
  return asImpl(Impl)->getTypeForRemoteTypeMetadata(address, skipArtificial);
}

Result<MetadataKind>
RemoteASTContext::getKindForRemoteTypeMetadata(remote::RemoteAddress address) {
  return asImpl(Impl)->getKindForRemoteTypeMetadata(address);
}

Result<NominalTypeDecl *>
RemoteASTContext::getDeclForRemoteNominalTypeDescriptor(RemoteAddress address) {
  return asImpl(Impl)->getDeclForRemoteNominalTypeDescriptor(address);
}

Result<uint64_t>
RemoteASTContext::getOffsetOfMember(Type type, RemoteAddress optMetadata,
                                    StringRef memberName) {
  return asImpl(Impl)->getOffsetOfMember(type, optMetadata, memberName);
}

Result<remote::RemoteAddress>
RemoteASTContext::getHeapMetadataForObject(remote::RemoteAddress address) {
  return asImpl(Impl)->getHeapMetadataForObject(address);
}

Result<OpenedExistential>
RemoteASTContext::getDynamicTypeAndAddressForError(
    remote::RemoteAddress address) {
  return asImpl(Impl)->getDynamicTypeAndAddressForError(address);
}

Result<OpenedExistential>
RemoteASTContext::getDynamicTypeAndAddressForExistential(
    remote::RemoteAddress address, Type staticType) {
  return asImpl(Impl)->getDynamicTypeAndAddressForExistential(address,
                                                              staticType);
}

Result<Type>
RemoteASTContext::getUnderlyingTypeForOpaqueType(
    remote::RemoteAddress opaqueDescriptor,
    SubstitutionMap substitutions,
    unsigned ordinal) {
  return asImpl(Impl)->getUnderlyingTypeForOpaqueType(opaqueDescriptor,
                                                      substitutions, ordinal);
}

//===--- TypeLayout.cpp - TypeLayout --------------------------------------===//
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
#include "TypeLayout.h"
#include "ConstantBuilder.h"
#include "EnumPayload.h"
#include "FixedTypeInfo.h"
#include "GenOpaque.h"
#include "IRGen.h"
#include "GenEnum.h"
#include "GenExistential.h"
#include "GenericArguments.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "SwitchBuilder.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/SIL/TypeLowering.h"
#include "llvm/ADT/None.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include <cstdint>

using namespace swift;
using namespace irgen;

namespace swift {
namespace irgen {

enum class LayoutStringFlags : uint64_t {
  Empty = 0,
  // TODO: Track other useful information tha can be used to optimize layout
  //       strings, like different reference kinds contained in the string
  //       number of ref counting operations (maybe up to 4), so we can
  //       use witness functions optimized for these cases.
  HasRelativePointers = (1ULL << 63),
};

inline bool operator&(LayoutStringFlags a, LayoutStringFlags b) {
  return (uint64_t(a) & uint64_t(b)) != 0;
}
inline LayoutStringFlags operator|(LayoutStringFlags a, LayoutStringFlags b) {
  return LayoutStringFlags(uint64_t(a) | uint64_t(b));
}
inline LayoutStringFlags &operator|=(LayoutStringFlags &a, LayoutStringFlags b) {
  return a = (a | b);
}

class LayoutStringBuilder {
public:
  enum class RefCountingKind : uint8_t {
    End = 0x00,
    Error = 0x01,
    NativeStrong = 0x02,
    NativeUnowned = 0x03,
    NativeWeak = 0x04,
    Unknown = 0x05,
    UnknownUnowned = 0x06,
    UnknownWeak = 0x07,
    Bridge = 0x08,
    Block = 0x09,
    ObjC = 0x0a,
    Custom = 0x0b,

    // reserved
    // Metatype = 0x0c,

    Existential = 0x0e,
    Resilient = 0x0f,
    SinglePayloadEnumSimple = 0x10,
    SinglePayloadEnumFN = 0x11,
    // reserved
    // SinglePayloadEnumFNResolved = 0x12,
    // SinglePayloadEnumGeneric = 0x13,

    MultiPayloadEnumFN = 0x14,
    // reserved
    // MultiPayloadEnumFNResolved = 0x15,
    // MultiPayloadEnumGeneric = 0x16,

    Skip = 0x80,
    // We may use the MSB as flag that a count follows,
    // so all following values are reserved
    // Reserved: 0x81 - 0xFF
  };

private:
  struct SinglePayloadEnumSimple {
    uint64_t zeroTagValue;
    unsigned xiTagValues;
    unsigned extraTagByteCount;
    unsigned xiByteCount;
    unsigned xiOffset;
    const TypeLayoutEntry *payload;
  };

  struct SinglePayloadEnumFN {
    llvm::Function *tagFn;
    unsigned extraTagByteCount;
    const TypeLayoutEntry *payload;
  };

  struct MultiPayloadEnumFN {
    llvm::Function *tagFn;
    const EnumTypeLayoutEntry *entry;
  };

  struct RefCounting {
    RefCountingKind kind;
    union {
      size_t size;
      llvm::Function *metaTypeRef;
      SinglePayloadEnumSimple singlePayloadEnumSimple;
      SinglePayloadEnumFN singlePayloadEnumFN;
      MultiPayloadEnumFN multiPayloadEnumFN;
    };

    RefCounting() = default;
  };

  std::vector<RefCounting> refCountings;

public:
  LayoutStringBuilder() = default;
  ~LayoutStringBuilder() = default;

  void addRefCount(RefCountingKind kind, size_t size) {
    RefCounting op;
    op.kind = kind;
    op.size = size;
    refCountings.push_back(op);
  }

  void addSinglePayloadEnumSimple(uint64_t zeroTagValue, unsigned xiTagValues,
                                  unsigned extraTagByteCount,
                                  unsigned xiByteCount, unsigned xiOffset,
                                  const TypeLayoutEntry *payload) {
    RefCounting op;
    op.kind = RefCountingKind::SinglePayloadEnumSimple;
    op.singlePayloadEnumSimple.zeroTagValue = zeroTagValue;
    op.singlePayloadEnumSimple.xiTagValues = xiTagValues;
    op.singlePayloadEnumSimple.extraTagByteCount = extraTagByteCount;
    op.singlePayloadEnumSimple.xiByteCount = xiByteCount;
    op.singlePayloadEnumSimple.xiOffset = xiOffset;
    op.singlePayloadEnumSimple.payload = payload;
    refCountings.push_back(op);
  }

  void addSinglePayloadEnumFN(llvm::Function *tagFn, unsigned extraTagByteCount,
                              const TypeLayoutEntry *payload) {
    RefCounting op;
    op.kind = RefCountingKind::SinglePayloadEnumFN;
    op.singlePayloadEnumFN.tagFn = tagFn;
    op.singlePayloadEnumSimple.extraTagByteCount = extraTagByteCount;
    op.singlePayloadEnumFN.payload = payload;
    refCountings.push_back(op);
  }

  void addMultiPayloadEnumFN(llvm::Function *tagFn,
                             const EnumTypeLayoutEntry *entry) {
    RefCounting op;
    op.kind = RefCountingKind::MultiPayloadEnumFN;
    op.multiPayloadEnumFN.tagFn = tagFn;
    op.multiPayloadEnumFN.entry = entry;
    refCountings.push_back(op);
  }

  void addSkip(size_t size) {
    if (refCountings.empty() ||
        refCountings.back().kind != RefCountingKind::Skip) {
      RefCounting op;
      op.kind = RefCountingKind::Skip;
      op.size = 0;
      refCountings.push_back(op);
    }
    auto &refCounting = refCountings.back();

    refCounting.size += size;
  }

  // TODO: this is a stopgap solution until we can
  // actually support enums
  void addFixedEnumRefCount(llvm::Function *metaTypeRef) {
    RefCounting op;
    op.kind = RefCountingKind::Resilient;
    op.metaTypeRef = metaTypeRef;
    refCountings.push_back(op);
  }

public:
  void addRefCountings(IRGenModule &IGM, ConstantStructBuilder &B,
                       GenericSignature genericSig,
                       const std::vector<RefCounting> &_refCountings,
                       size_t &skip, size_t &refCountBytes,
                       LayoutStringFlags &flags) const {
    for (auto &refCounting : _refCountings) {
      switch (refCounting.kind) {
      case RefCountingKind::Skip:
        skip += refCounting.size;
        break;

      case RefCountingKind::Resilient: {
        uint64_t op = (static_cast<uint64_t>(refCounting.kind) << 56) | skip;
        B.addInt64(op);
        // We are not using a compact pointer here, because when instantiating
        // a layout string, we determine the size of the new string by the
        // size of its parts. On instantiation we also resolve this pointer
        // to a full metadata pointer, which always has full pointer size.
        B.addRelativeOffset(IGM.IntPtrTy, refCounting.metaTypeRef);
        refCountBytes += sizeof(uint64_t) + IGM.getPointerSize().getValue();

        flags |= LayoutStringFlags::HasRelativePointers;
        skip = 0;
        break;
      }

      case RefCountingKind::SinglePayloadEnumSimple: {
        uint64_t op = (static_cast<uint64_t>(refCounting.kind) << 56) | skip;
        B.addInt64(op);

        skip = 0;
        size_t nestedRefCountBytes = 0;

        auto enumData = refCounting.singlePayloadEnumSimple;
        assert(enumData.extraTagByteCount <= 4);

        // We use power of 2 sizes, so we compact them to save space
        // Formula to read the actual size: x == 0 ? 0 : 1 << (x - 1);
        uint64_t compactExtraTagByteCount =
            std::min(enumData.extraTagByteCount, 3u);
        uint64_t compactXIByteCount =
            enumData.xiByteCount == 8 ? 4 : std::min(enumData.xiByteCount, 3u);
        uint64_t byteCountsAndOffset = compactExtraTagByteCount << 62 |
                                       compactXIByteCount << 59 |
                                       enumData.xiOffset;
        B.addInt64(byteCountsAndOffset);
        B.addSize(enumData.payload->fixedSize(IGM).value());
        B.addInt64(enumData.zeroTagValue);
        B.addSize(Size(enumData.xiTagValues));
        auto nestedRefCountBytesPlaceholder =
            B.addPlaceholderWithSize(IGM.SizeTy);
        auto skipBytesPlaceholder = B.addPlaceholderWithSize(IGM.SizeTy);

        LayoutStringBuilder nestedBuilder{};
        enumData.payload->refCountString(IGM, nestedBuilder, genericSig);
        addRefCountings(IGM, B, genericSig, nestedBuilder.refCountings, skip,
                        nestedRefCountBytes, flags);

        auto nestedSkip = enumData.payload->fixedSize(IGM)->getValue() - skip;
        B.fillPlaceholderWithInt(nestedRefCountBytesPlaceholder, IGM.SizeTy,
                                 nestedRefCountBytes);
        B.fillPlaceholderWithInt(skipBytesPlaceholder, IGM.SizeTy, nestedSkip);

        refCountBytes += (3 * sizeof(uint64_t)) +
                         (4 * IGM.getPointerSize().getValue()) +
                         nestedRefCountBytes;

        skip += enumData.extraTagByteCount;

        break;
      }

      case RefCountingKind::SinglePayloadEnumFN: {
        uint64_t op = (static_cast<uint64_t>(refCounting.kind) << 56) | skip;
        B.addInt64(op);

        skip = 0;
        size_t nestedRefCountBytes = 0;

        auto enumData = refCounting.singlePayloadEnumFN;

        B.addRelativeOffset(IGM.IntPtrTy, enumData.tagFn);

        auto nestedRefCountBytesPlaceholder =
            B.addPlaceholderWithSize(IGM.SizeTy);
        auto skipBytesPlaceholder = B.addPlaceholderWithSize(IGM.SizeTy);

        LayoutStringBuilder nestedBuilder{};
        enumData.payload->refCountString(IGM, nestedBuilder, genericSig);
        addRefCountings(IGM, B, genericSig, nestedBuilder.refCountings, skip,
                        nestedRefCountBytes, flags);

        auto nestedSkip = enumData.payload->fixedSize(IGM)->getValue() - skip;
        B.fillPlaceholderWithInt(nestedRefCountBytesPlaceholder, IGM.SizeTy,
                                 nestedRefCountBytes);
        B.fillPlaceholderWithInt(skipBytesPlaceholder, IGM.SizeTy, nestedSkip);

        refCountBytes += (sizeof(uint64_t)) +
                         (3 * IGM.getPointerSize().getValue()) +
                         nestedRefCountBytes;
        skip += enumData.extraTagByteCount;

        flags |= LayoutStringFlags::HasRelativePointers;
        break;
      }

      case RefCountingKind::MultiPayloadEnumFN: {
        uint64_t op = (static_cast<uint64_t>(refCounting.kind) << 56) | skip;
        B.addInt64(op);

        skip = 0;

        auto enumData = refCounting.multiPayloadEnumFN;
        auto payloads = enumData.entry->cases;

        B.addRelativeOffset(IGM.IntPtrTy, enumData.tagFn);

        B.addSize(Size(payloads.size()));

        auto nestedRefCountBytesPlaceholder =
            B.addPlaceholderWithSize(IGM.SizeTy);
        B.addSize(*enumData.entry->fixedSize(IGM));

        SmallVector<
            clang::CodeGen::ConstantAggregateBuilderBase::PlaceholderPosition,
            4>
            offsetPlaceholders;
        for (auto *p : payloads) {
          (void)p;
          auto placeholder = B.addPlaceholderWithSize(IGM.SizeTy);
          offsetPlaceholders.push_back(placeholder);
          refCountBytes += IGM.getPointerSize().getValue();
        }

        size_t nestedRefCountBytes = 0;
        for (auto p : llvm::zip(payloads, offsetPlaceholders)) {
          auto *payload = std::get<0>(p);

          B.fillPlaceholderWithInt(std::get<1>(p), IGM.SizeTy,
                                   nestedRefCountBytes);

          size_t nestedSkip = 0;
          LayoutStringBuilder nestedBuilder{};
          payload->refCountString(IGM, nestedBuilder, genericSig);
          addRefCountings(IGM, B, genericSig, nestedBuilder.refCountings,
                          nestedSkip, nestedRefCountBytes, flags);

          // NUL terminator
          B.addInt64(0);
          nestedRefCountBytes += sizeof(uint64_t);
        }

        B.fillPlaceholderWithInt(nestedRefCountBytesPlaceholder, IGM.SizeTy,
                                 nestedRefCountBytes);

        refCountBytes += sizeof(uint64_t) +
                         (4 * IGM.getPointerSize().getValue()) +
                         nestedRefCountBytes;

        flags |= LayoutStringFlags::HasRelativePointers;
        break;
      }

      default: {
        uint64_t op = (static_cast<uint64_t>(refCounting.kind) << 56) | skip;
        B.addInt64(op);
        refCountBytes += sizeof(uint64_t);

        skip = refCounting.size;
        break;
      }
      }
    }
  }

  void result(IRGenModule &IGM, ConstantStructBuilder &B,
              GenericSignature genericSig) const {
    auto flagsPlaceholder = B.addPlaceholderWithSize(IGM.Int64Ty);
    auto sizePlaceholder = B.addPlaceholderWithSize(IGM.SizeTy);
    size_t skip = 0;
    size_t refCountBytes = 0;
    LayoutStringFlags flags = LayoutStringFlags::Empty;

    addRefCountings(IGM, B, genericSig, refCountings, skip, refCountBytes,
                    flags);

    // size of ref counting ops in bytes
    B.fillPlaceholderWithInt(flagsPlaceholder, IGM.Int64Ty,
                             static_cast<uint64_t>(flags));
    B.fillPlaceholderWithInt(sizePlaceholder, IGM.SizeTy, refCountBytes);

    B.addInt64(skip);

    // NUL terminator
    B.addInt64(0);
  }
};
}
}

ScalarKind swift::irgen::refcountingToScalarKind(ReferenceCounting refCounting) {
  switch (refCounting) {
  case ReferenceCounting::Native:
    return ScalarKind::NativeStrongReference;
  case ReferenceCounting::Bridge:
    return ScalarKind::BridgeReference;
  case ReferenceCounting::Block:
    return ScalarKind::BlockReference;
  case ReferenceCounting::Error:
    return ScalarKind::ErrorReference;
  case ReferenceCounting::Unknown:
    return ScalarKind::UnknownReference;
  case ReferenceCounting::ObjC:
    return ScalarKind::ObjCReference;
  case ReferenceCounting::None:
    return ScalarKind::TriviallyDestroyable;
  case ReferenceCounting::Custom:
    return ScalarKind::CustomReference;
  }
}

static bool isNullableRefCounted(ScalarKind kind) {
  switch (kind) {
  case ScalarKind::ErrorReference:
  case ScalarKind::NativeStrongReference:
  case ScalarKind::NativeUnownedReference:
  case ScalarKind::NativeWeakReference:
  case ScalarKind::UnknownUnownedReference:
  case ScalarKind::UnknownWeakReference:
  case ScalarKind::UnknownReference:
  case ScalarKind::BlockReference:
  case ScalarKind::BridgeReference:
  case ScalarKind::ObjCReference:
    return true;
  default:
    return false;
  }
}

static std::string scalarToString(ScalarKind kind) {
  switch (kind) {
  case ScalarKind::ErrorReference: return "ErrorReference";
  case ScalarKind::NativeStrongReference: return "NativeStrongReference";
  case ScalarKind::NativeUnownedReference: return "NativeUnownedReference";
  case ScalarKind::NativeWeakReference: return "NativeWeakReference";
  case ScalarKind::UnknownUnownedReference: return "UnknownUnownedReference";
  case ScalarKind::UnknownWeakReference: return "UnknownWeakReference";
  case ScalarKind::UnknownReference: return "UnknownReference";
  case ScalarKind::BlockReference: return "BlockReference";
  case ScalarKind::BridgeReference: return "BridgeReference";
  case ScalarKind::ObjCReference: return "ObjCReference";
  case ScalarKind::TriviallyDestroyable: return "TriviallyDestroyable";
  case ScalarKind::Immovable: return "Immovable";
  case ScalarKind::BlockStorage: return "BlockStorage";
  case ScalarKind::ThickFunc: return "ThickFunc";
  case ScalarKind::ExistentialReference: return "ExistentialReference";
  case ScalarKind::CustomReference: return "Custom";
  }
}

llvm::Function *createMetatypeAccessorFunction(IRGenModule &IGM, SILType ty,
                                               GenericSignature genericSig) {
  CanType fieldType = ty.getASTType();

  auto sig = genericSig.getCanonicalSignature();
  IRGenMangler mangler;
  std::string symbolName =
      mangler.mangleSymbolNameForMangledMetadataAccessorString(
          "get_type_metadata_for_layout_string", sig,
          fieldType->mapTypeOutOfContext()->getCanonicalType());

  auto helperFn = IGM.getOrCreateHelperFunction(
      symbolName, IGM.TypeMetadataPtrTy /*retTy*/,
      IGM.TypeMetadataPtrPtrTy /*argTys*/, [&](IRGenFunction &IGF) {
        if (genericSig) {
          auto genericEnv = genericSig.getGenericEnvironment();
          SmallVector<GenericRequirement, 4> requirements;

          enumerateGenericSignatureRequirements(
              sig,
              [&](GenericRequirement reqt) { requirements.push_back(reqt); });

          auto bindingsBufPtr = IGF.collectParameters().claimNext();
          bindFromGenericRequirementsBuffer(
              IGF, requirements,
              Address(bindingsBufPtr, IGM.TypeMetadataPtrTy,
                      IGM.getPointerAlignment()),
              MetadataState::Complete,
              genericEnv->getForwardingSubstitutionMap());
        }

        auto ret = IGF.emitTypeMetadataRefForLayout(ty);
        IGF.Builder.CreateRet(ret);
      });

  return static_cast<llvm::Function *>(helperFn);
}

llvm::Function *createFixedEnumLoadTag(IRGenModule &IGM,
                                       const EnumTypeLayoutEntry &entry) {
  assert(entry.isFixedSize(IGM));

  IRGenMangler mangler;
  auto symbol = mangler.mangleSymbolNameForMangledGetEnumTagForLayoutString(
      entry.ty.getASTType()->mapTypeOutOfContext()->getCanonicalType());

  auto helperFn = IGM.getOrCreateHelperFunction(
      symbol, IGM.Int32Ty /*retTy*/, IGM.Int8PtrTy /*argTys*/,
      [&](IRGenFunction &IGF) {
        auto enumPtr = IGF.collectParameters().claimNext();
        auto *typeInfo = *entry.fixedTypeInfo;
        auto enumType = typeInfo->getStorageType()->getPointerTo();
        auto castEnumPtr = IGF.Builder.CreateBitCast(enumPtr, enumType);
        auto enumAddr = typeInfo->getAddressForPointer(castEnumPtr);

        auto &strategy = getEnumImplStrategy(IGM, entry.ty);
        auto tag = strategy.emitGetEnumTag(IGF, entry.ty, enumAddr);
        IGF.Builder.CreateRet(tag);
      });

  return static_cast<llvm::Function *>(helperFn);
}

TypeLayoutEntry::~TypeLayoutEntry() {}

void TypeLayoutEntry::computeProperties() {
  // does not add anything.
}

void TypeLayoutEntry::gatherProperties(TypeLayoutEntry *fromEntry) {
  hasArchetypeField |= fromEntry->hasArchetypeField;
  hasResilientField |= fromEntry->hasResilientField;
  hasDependentResilientField |= fromEntry->hasDependentResilientField;

  assert(!(!hasResilientField && hasDependentResilientField));
}

const EnumTypeLayoutEntry *TypeLayoutEntry::getAsEnum() const {
  if (getKind() == TypeLayoutEntryKind::Enum) {
    return static_cast<const EnumTypeLayoutEntry *>(this);
  }
  return nullptr;
}

bool TypeLayoutEntry::isAlignedGroup() const {
  return getKind() == TypeLayoutEntryKind::AlignedGroup;
}

llvm::Optional<const FixedTypeInfo *>
TypeLayoutEntry::getFixedTypeInfo() const {
  return llvm::None;
}

llvm::Value *TypeLayoutEntry::alignmentMask(IRGenFunction &IGF) const {
  assert(isEmpty());
  return IGF.IGM.getSize(Size(0));
}

llvm::Value *TypeLayoutEntry::size(IRGenFunction &IGF) const {
  assert(isEmpty());
  return IGF.IGM.getSize(Size(0));
}

bool TypeLayoutEntry::isFixedSize(IRGenModule &IGM) const {
  assert(isEmpty() &&
         "Type isn't empty -- perhaps you forgot to override this function?");
  return true;
}

bool TypeLayoutEntry::isSingleRetainablePointer() const {
  assert(isEmpty() &&
         "Type isn't empty -- perhaps you forgot to override this function?");
  return false;
}

bool TypeLayoutEntry::isTriviallyDestroyable() const {
  assert(isEmpty() &&
         "Type isn't empty -- perhaps you forgot to override this function?");
  return true;
}

bool TypeLayoutEntry::canValueWitnessExtraInhabitantsUpTo(
    IRGenModule &IGM, unsigned index) const {
  assert(isEmpty() &&
         "Type isn't empty -- perhaps you forgot to override this function?");
  return index == 0;
}

llvm::Optional<Size> TypeLayoutEntry::fixedSize(IRGenModule &IGM) const {
  assert(isEmpty() &&
         "Type isn't empty -- perhaps you forgot to override this function?");
  return Size(0);
}

llvm::Optional<Alignment>
TypeLayoutEntry::fixedAlignment(IRGenModule &IGM) const {
  assert(isEmpty() &&
         "Type isn't empty -- perhaps you forgot to override this function?");
  return Alignment();
}

llvm::Optional<uint32_t> TypeLayoutEntry::fixedXICount(IRGenModule &IGM) const {
  assert(isEmpty() &&
         "Type isn't empty -- perhaps you forgot to override this function?");
  return 0;
}

llvm::Value *TypeLayoutEntry::isBitwiseTakable(IRGenFunction &IGF) const {
  assert(isEmpty());
  return llvm::ConstantInt::get(IGF.IGM.Int1Ty, true);
}

llvm::Constant *
TypeLayoutEntry::layoutString(IRGenModule &IGM,
                              GenericSignature genericSig) const {
  assert(isEmpty());
  return nullptr;
}

bool TypeLayoutEntry::refCountString(IRGenModule &IGM, LayoutStringBuilder &B,
                                     GenericSignature genericSig) const {
  assert(isEmpty());
  return true;
}

llvm::Value *TypeLayoutEntry::extraInhabitantCount(IRGenFunction &IGF) const {
  assert(isEmpty());
  return llvm::ConstantInt::get(IGF.IGM.Int32Ty, 0);
}

void TypeLayoutEntry::destroy(IRGenFunction &IGF, Address addr) const {
  assert(isEmpty());
  // Nothing to destroy.
}

void TypeLayoutEntry::assign(IRGenFunction &IGF, Address dest, Address src,
                             IsTake_t isTake) const {
  if (isTake == IsTake) {
    assignWithTake(IGF, dest, src);
  } else {
    assignWithCopy(IGF, dest, src);
  }
}

void TypeLayoutEntry::initialize(IRGenFunction &IGF, Address dest, Address src,
                                 IsTake_t isTake) const {
  if (isTake == IsTake) {
    initWithTake(IGF, dest, src);
  } else {
    initWithCopy(IGF, dest, src);
  }
}

void TypeLayoutEntry::assignWithCopy(IRGenFunction &IGF, Address dest,
                                     Address src) const {
  assert(isEmpty());
  // Nothing to copy.
}

void TypeLayoutEntry::assignWithTake(IRGenFunction &IGF, Address dest,
                                     Address src) const {
  assert(isEmpty());
  // Nothing to copy.
}

void TypeLayoutEntry::initWithCopy(IRGenFunction &IGF, Address dest,
                                     Address src) const {
  assert(isEmpty());
  // Nothing to copy.
}

void TypeLayoutEntry::initWithTake(IRGenFunction &IGF, Address dest,
                                   Address src) const {
  assert(isEmpty());
  // Nothing to copy.
}

bool TypeLayoutEntry::containsResilientField() const {
  return hasResilientField;
}

bool TypeLayoutEntry::containsArchetypeField() const {
  return hasArchetypeField;
}

bool TypeLayoutEntry::containsDependentResilientField() const {
  return hasDependentResilientField;
}

llvm::Value *TypeLayoutEntry::getEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *numEmptyCases, Address addr) const {
  assert(isEmpty());
  return getFixedTypeEnumTagSinglePayload(
      IGF, numEmptyCases, addr, IGF.IGM.getSize(Size(0)), Size(0), 0,
      [](Address addr) -> llvm::Value * {
        // This function should not be called since the
        // fixedExtraInhabitantCount is zero. We should just store to the extra
        // tag bytes.
        llvm_unreachable("this function should not be called");
        return nullptr;
      },
      true);
}

void TypeLayoutEntry::storeEnumTagSinglePayload(IRGenFunction &IGF,
                                                llvm::Value *tag,
                                                llvm::Value *numEmptyCases,
                                                Address addr) const {
  assert(isEmpty());
  storeFixedTypeEnumTagSinglePayload(
      IGF, tag, numEmptyCases, addr, IGF.IGM.getSize(Size(0)), Size(0), 0,
      [&](llvm::Value *, Address) {
        // This function should not be called since the
        // fixedExtraInhabitantCount is zero. We should just store to the extra
        // tag bytes.
        llvm_unreachable("this function should not be called");
      },
      true);
}

struct EnumTagInfo {
  llvm::Value *numTags;
  llvm::Value *numTagBytes;
};

static EnumTagInfo getEnumTagBytes(IRGenFunction &IGF, llvm::Value *size,
                                   llvm::Value *emptyCases,
                                   llvm::Value *payloadCases) {
  // Implements (compare getEnumTagCounts):
  // unsigned numTags = payloadCases;
  // if (emptyCases > 0) {
  //   if (size >= 4)
  //     numTags += 1;
  //   else {
  //     unsigned bits = size * 8U;
  //     unsigned casesPerTagBitValue = 1U << bits;
  //     numTags += ((emptyCases + (casesPerTagBitValue-1U)) >> bits);
  //   }
  // }
  // unsigned numTagBytes = (numTags <=    1 ? 0 :
  //                         numTags <   256 ? 1 :
  //                         numTags < 65536 ? 2 : 4);
  // return numTagBytes;

  auto &Builder = IGF.Builder;
  auto &IGM = IGF.IGM;
  auto &ctx = IGM.getLLVMContext();

  // unsigned numTags = payloadCases;
  auto numTags = payloadCases;
  auto block1 = Builder.GetInsertBlock();
  auto zero = IGM.getInt32(0);
  auto one = IGM.getInt32(1);

  auto someEmptyCasesBB = llvm::BasicBlock::Create(ctx);
  auto noEmptyCasesBB = llvm::BasicBlock::Create(ctx);
  auto someEmptyCases = Builder.CreateICmpUGT(emptyCases, zero);
  // if (emptyCases > 0) {
  Builder.CreateCondBr(someEmptyCases, someEmptyCasesBB, noEmptyCasesBB);

  Builder.emitBlock(someEmptyCasesBB);
  auto someEmptyCasesMergeBB = llvm::BasicBlock::Create(ctx);
  auto gte4BB = llvm::BasicBlock::Create(ctx);
  auto lt4BB = llvm::BasicBlock::Create(ctx);
  auto sizeGTE4 = Builder.CreateICmpUGE(size, IGM.getInt32(4));
  // if (size >= 4) {
  Builder.CreateCondBr(sizeGTE4, gte4BB, lt4BB);

  Builder.emitBlock(gte4BB);
  //   numTags += 1;
  auto numTagsPlusOne = Builder.CreateAdd(numTags, one);
  Builder.CreateBr(someEmptyCasesMergeBB);

  // } else {
  Builder.emitBlock(lt4BB);
  //   unsigned bits = size * 8U;
  //   unsigned casesPerTagBitValue = 1U << bits;
  //   numTags += ((emptyCases + (casesPerTagBitValue-1U)) >> bits);
  auto *bits = Builder.CreateMul(size, IGM.getInt32(8));
  auto *casesPerTagBitValue = Builder.CreateShl(one, bits);
  auto *numTags2 = Builder.CreateSub(casesPerTagBitValue, one);
  numTags2 = Builder.CreateAdd(numTags2, emptyCases);
  numTags2 = Builder.CreateLShr(numTags2, bits);
  numTags2 = Builder.CreateAdd(numTags2, numTags);
  Builder.CreateBr(someEmptyCasesMergeBB);

  Builder.emitBlock(someEmptyCasesMergeBB);
  auto numTagsSomeEmptyCases = Builder.CreatePHI(IGM.Int32Ty, 2);
  numTagsSomeEmptyCases->setName("num-tags-some-empty-cases");
  numTagsSomeEmptyCases->addIncoming(numTagsPlusOne, gte4BB);
  numTagsSomeEmptyCases->addIncoming(numTags2, lt4BB);
  Builder.CreateBr(noEmptyCasesBB);

  Builder.emitBlock(noEmptyCasesBB);
  auto numTagsPhi = Builder.CreatePHI(IGM.Int32Ty, 2);
  numTagsPhi->setName("num-tags-phi");
  numTagsPhi->addIncoming(numTags, block1);
  numTagsPhi->addIncoming(numTagsSomeEmptyCases, someEmptyCasesMergeBB);

  // unsigned numTagBytes = (numTags <=    1 ? 0 :
  //                         numTags <   256 ? 1 :
  //                         numTags < 65536 ? 2 : 4);
  auto numTagsLTE1 = Builder.CreateICmpULE(numTagsPhi, one);
  auto numTagsLT256 = Builder.CreateICmpULT(numTagsPhi, IGM.getInt32(256));
  auto numTagsLT65536 =
      Builder.CreateICmpULT(numTagsPhi, IGM.getInt32(65536));
  auto useTwoOrFourByte =
      Builder.CreateSelect(numTagsLT65536, IGM.getInt32(2), IGM.getInt32(4));
  auto useOneTwoOrFourByte =
      Builder.CreateSelect(numTagsLT256, one, useTwoOrFourByte);
  auto numTagBytes =
      Builder.CreateSelect(numTagsLTE1, zero, useOneTwoOrFourByte);
  numTagBytes->setName("num-tag-bytes");
  return {numTagsPhi, numTagBytes};
}

struct FixedEnumTagInfo {
  uint32_t numTags;
  uint32_t numTagBytes;
};

///  Given the size of of an enum, number of empty and payload cases of an enum,
///  figure out how many tags we need, and how big those tags are.
static FixedEnumTagInfo getFixedEnumTagBytes(Size size, uint32_t emptyCases,
                                             uint32_t payloadCases) {
  unsigned numTags = payloadCases;
  if (emptyCases > 0) {
    if (size.getValue() >= 4)
      numTags += 1;
    else {
      unsigned bits = size.getValue() * 8U;
      unsigned casesPerTagBitValue = 1U << bits;
      numTags += ((emptyCases + (casesPerTagBitValue - 1U)) >> bits);
    }
  }
  unsigned numTagBytes = (numTags <= 1      ? 0
                          : numTags < 256   ? 1
                          : numTags < 65536 ? 2
                                            : 4);
  return {numTags, numTagBytes};
}

llvm::Value *TypeLayoutEntry::getEnumTagSinglePayloadGeneric(
    IRGenFunction &IGF, Address addr, llvm::Value *numEmptyCases,
    llvm::function_ref<llvm::Value *(Address addr)> getExtraInhabitantIndexFun)
    const {
  auto &IGM = IGF.IGM;
  auto &Ctx = IGF.IGM.getLLVMContext();
  auto &Builder = IGF.Builder;

  auto numExtraInhabitants = this->extraInhabitantCount(IGF);
  auto size = this->size(IGF);

  auto *zero = llvm::ConstantInt::get(IGM.Int32Ty, 0U);
  auto *one = llvm::ConstantInt::get(IGM.Int32Ty, 1U);
  auto *four = llvm::ConstantInt::get(IGM.Int32Ty, 4U);
  auto *eight = llvm::ConstantInt::get(IGM.Int32Ty, 8U);

  auto *extraTagBitsBB = llvm::BasicBlock::Create(Ctx);
  auto *noExtraTagBitsBB = llvm::BasicBlock::Create(Ctx);
  auto *hasEmptyCasesBB = llvm::BasicBlock::Create(Ctx);
  auto *singleCaseEnumBB = llvm::BasicBlock::Create(Ctx);

  auto *truncSize = Builder.CreateZExtOrTrunc(size, IGM.Int32Ty);

  // No empty cases so we must be the payload.
  auto *hasNoEmptyCases = Builder.CreateICmpEQ(zero, numEmptyCases);
  Builder.CreateCondBr(hasNoEmptyCases, singleCaseEnumBB, hasEmptyCasesBB);

  // Otherwise, check whether we need extra tag bits.
  Builder.emitBlock(hasEmptyCasesBB);
  auto *hasExtraTagBits =
      Builder.CreateICmpUGT(numEmptyCases, numExtraInhabitants);
  Builder.CreateCondBr(hasExtraTagBits, extraTagBitsBB, noExtraTagBitsBB);

  // There are extra tag bits to check.
  Builder.emitBlock(extraTagBitsBB);

  // Compute the number of extra tag bytes.
  auto *emptyCases = Builder.CreateSub(numEmptyCases, numExtraInhabitants);
  auto *numExtraTagBytes =
      getEnumTagBytes(IGF, truncSize, emptyCases, IGM.getInt32(1)).numTagBytes;

  // Read the value stored in the extra tag bytes.
  auto *valueAddr =
      Builder.CreateBitOrPointerCast(addr.getAddress(), IGM.Int8PtrTy);
  auto *extraTagBitsAddr =
      Builder.CreateInBoundsGEP(IGM.Int8Ty, valueAddr, size);
  auto *extraTagBits =
      emitGetTag(IGF, Address(extraTagBitsAddr, IGM.Int8Ty, Alignment(1)),
                 numExtraTagBytes);

  extraTagBitsBB = llvm::BasicBlock::Create(Ctx);
  Builder.CreateCondBr(Builder.CreateICmpEQ(extraTagBits, zero),
                       noExtraTagBitsBB, extraTagBitsBB);

  auto *resultBB = llvm::BasicBlock::Create(Ctx);

  Builder.emitBlock(extraTagBitsBB);

  auto sizeGTE4 = Builder.CreateICmpUGE(truncSize, four);
  auto *caseIndexFromExtraTagBits = Builder.CreateSelect(
      sizeGTE4, zero,
      Builder.CreateShl(Builder.CreateSub(extraTagBits, one),
                        Builder.CreateMul(eight, truncSize)));

  auto caseIndexFromValue = llvm::PHINode::Create(IGM.Int32Ty, 2);
  caseIndexFromValue->setName("case-index-from-value");
  auto contBB = IGF.createBasicBlock("");
  auto nonZeroSizeBB = IGF.createBasicBlock("");
  auto isNonZero = Builder.CreateICmpNE(truncSize, zero);
  caseIndexFromValue->addIncoming(zero, Builder.GetInsertBlock());
  Builder.CreateCondBr(isNonZero, nonZeroSizeBB, contBB);
  {
    // Read up to one pointer-sized 'chunk' of the payload.
    // The size of the chunk does not have to be a power of 2.
    Builder.emitBlock(nonZeroSizeBB);
    auto sizeClampedTo4 = Builder.CreateSelect(sizeGTE4, four, truncSize);
    auto loadPayloadChunk = emitLoad1to4Bytes(IGF, addr, sizeClampedTo4);
    caseIndexFromValue->addIncoming(loadPayloadChunk, Builder.GetInsertBlock());
    Builder.CreateBr(contBB);
  }
  Builder.emitBlock(contBB);
  Builder.Insert(caseIndexFromValue);

  auto *result1 = Builder.CreateAdd(
      numExtraInhabitants,
      Builder.CreateOr(caseIndexFromValue, caseIndexFromExtraTagBits));
  result1 = Builder.CreateAdd(result1, one);
  auto *result1BB = Builder.GetInsertBlock();
  Builder.CreateBr(resultBB);

  // Extra tag bits were considered and zero or there are not extra tag
  // bits.
  Builder.emitBlock(noExtraTagBitsBB);

  // If there are extra inhabitants, see whether the payload is valid.
  auto result0 = llvm::PHINode::Create(IGM.Int32Ty, 2);
  result0->setName("get-payload-tag-phi");
  auto contBB2 = IGF.createBasicBlock("");
  auto hasXIBB = IGF.createBasicBlock("");
  auto isNonZeroXI = Builder.CreateICmpNE(numExtraInhabitants, zero);
  result0->addIncoming(zero, Builder.GetInsertBlock());
  Builder.CreateCondBr(isNonZeroXI, hasXIBB, contBB2);
  {
    Builder.emitBlock(hasXIBB);
    ConditionalDominanceScope scope(IGF);
    // Get tag in payload.
    auto tagInPayload = getExtraInhabitantIndexFun(addr);
    result0->addIncoming(tagInPayload, Builder.GetInsertBlock());
    Builder.CreateBr(contBB2);
  }
  Builder.emitBlock(contBB2);
  Builder.Insert(result0);
  auto result0BB = Builder.GetInsertBlock();
  Builder.CreateBr(resultBB);

  Builder.emitBlock(singleCaseEnumBB);
  // Otherwise, we have a valid payload.
  auto *result2 = zero;
  Builder.CreateBr(resultBB);

  Builder.emitBlock(resultBB);
  auto *result = Builder.CreatePHI(IGM.Int32Ty, 3);
  result->addIncoming(result0, result0BB);
  result->addIncoming(result1, result1BB);
  result->addIncoming(result2, singleCaseEnumBB);
  return result;
}

void TypeLayoutEntry::storeEnumTagSinglePayloadGeneric(
    IRGenFunction &IGF, llvm::Value *tag, llvm::Value *numEmptyCases,
    Address addr,
    llvm::function_ref<void(Address addr, llvm::Value *tag)>
        storeExtraInhabitantIndexFun) const {
  auto &IGM = IGF.IGM;
  auto &Ctx = IGF.IGM.getLLVMContext();
  auto &Builder = IGF.Builder;

  auto numExtraInhabitants = this->extraInhabitantCount(IGF);
  auto size = this->size(IGF);

  auto *truncSize = Builder.CreateZExtOrTrunc(size, IGM.Int32Ty);
  auto &int32Ty = IGM.Int32Ty;
  auto *zero = llvm::ConstantInt::get(int32Ty, 0U);
  auto *one = llvm::ConstantInt::get(int32Ty, 1U);
  auto *four = llvm::ConstantInt::get(int32Ty, 4U);
  auto *eight = llvm::ConstantInt::get(int32Ty, 8U);

  auto *valueAddr =
      Builder.CreateBitOrPointerCast(addr.getAddress(), IGM.Int8PtrTy);
  auto extraTagBitsAddr =
      Address(Builder.CreateInBoundsGEP(IGM.Int8Ty, valueAddr, size),
              IGM.Int8Ty, Alignment(1));

  // Do we need extra tag bytes.
  auto *entryBB = Builder.GetInsertBlock();
  auto *continueBB = llvm::BasicBlock::Create(Ctx);
  auto *computeExtraTagBytesBB = llvm::BasicBlock::Create(Ctx);
  auto *hasExtraTagBits =
      Builder.CreateICmpUGT(numEmptyCases, numExtraInhabitants);
  Builder.CreateCondBr(hasExtraTagBits, computeExtraTagBytesBB, continueBB);

  Builder.emitBlock(computeExtraTagBytesBB);
  // Compute the number of extra tag bytes.
  auto *emptyCases = Builder.CreateSub(numEmptyCases, numExtraInhabitants);
  auto *numExtraTagBytes0 =
      getEnumTagBytes(IGF, truncSize, emptyCases, IGM.getInt32(1)).numTagBytes;
  computeExtraTagBytesBB = Builder.GetInsertBlock();
  Builder.CreateBr(continueBB);

  Builder.emitBlock(continueBB);
  auto *numExtraTagBytes = Builder.CreatePHI(int32Ty, 2);
  numExtraTagBytes->addIncoming(zero, entryBB);
  numExtraTagBytes->addIncoming(numExtraTagBytes0, computeExtraTagBytesBB);

  // Check whether we need to set the extra tag bits to non zero.
  auto *isExtraTagBitsCaseBB = llvm::BasicBlock::Create(Ctx);
  auto *isPayloadOrInhabitantCaseBB = llvm::BasicBlock::Create(Ctx);
  auto *isPayloadOrExtraInhabitant =
      Builder.CreateICmpULE(tag, numExtraInhabitants);
  Builder.CreateCondBr(isPayloadOrExtraInhabitant, isPayloadOrInhabitantCaseBB,
                       isExtraTagBitsCaseBB);

  // We are the payload or fit within the extra inhabitants.
  Builder.emitBlock(isPayloadOrInhabitantCaseBB);
  // Zero the tag bits.
  emitSetTag(IGF, extraTagBitsAddr, zero, numExtraTagBytes);
  isPayloadOrInhabitantCaseBB = Builder.GetInsertBlock();
  auto *storeInhabitantBB = llvm::BasicBlock::Create(Ctx);
  auto *returnBB = llvm::BasicBlock::Create(Ctx);
  auto *isPayload = Builder.CreateICmpEQ(tag, zero);
  Builder.CreateCondBr(isPayload, returnBB, storeInhabitantBB);

  Builder.emitBlock(storeInhabitantBB);
  auto contBB2 = IGF.createBasicBlock("");
  auto hasXIBB = IGF.createBasicBlock("");
  auto isNonZeroXI = Builder.CreateICmpNE(numExtraInhabitants, zero);
  Builder.CreateCondBr(isNonZeroXI, hasXIBB, contBB2);
  {
    Builder.emitBlock(hasXIBB);
    // Store the extra inhabitant.
    storeExtraInhabitantIndexFun(addr, tag);
    Builder.CreateBr(contBB2);
  }
  Builder.emitBlock(contBB2);
  Builder.CreateBr(returnBB);

  // There are extra tag bits to consider.
  Builder.emitBlock(isExtraTagBitsCaseBB);

  // Write the extra tag bytes. At this point we know we have an no payload case
  // and therefore the index we should store is in the range
  // [0..ElementsWithNoPayload-1].
  auto *nonPayloadElementIndex = Builder.CreateSub(tag, one);
  auto *caseIndex =
      Builder.CreateSub(nonPayloadElementIndex, numExtraInhabitants);
  auto *isFourBytesPayload = Builder.CreateICmpUGE(truncSize, four);
  auto *payloadGE4BB = Builder.GetInsertBlock();
  auto *payloadLT4BB = llvm::BasicBlock::Create(Ctx);
  continueBB = llvm::BasicBlock::Create(Ctx);
  Builder.CreateCondBr(isFourBytesPayload, continueBB, payloadLT4BB);

  Builder.emitBlock(payloadLT4BB);
  auto *payloadBits = Builder.CreateMul(truncSize, eight);
  auto *extraTagIndex0 = Builder.CreateLShr(caseIndex, payloadBits);
  extraTagIndex0 = Builder.CreateAdd(one, extraTagIndex0);
  auto *payloadIndex0 = Builder.CreateShl(one, payloadBits);
  payloadIndex0 = Builder.CreateSub(payloadIndex0, one);
  payloadIndex0 = Builder.CreateAnd(payloadIndex0, caseIndex);
  Builder.CreateBr(continueBB);

  Builder.emitBlock(continueBB);
  auto *extraTagIndex = Builder.CreatePHI(int32Ty, 2);
  extraTagIndex->addIncoming(llvm::ConstantInt::get(int32Ty, 1), payloadGE4BB);
  extraTagIndex->addIncoming(extraTagIndex0, payloadLT4BB);

  auto *payloadIndex = Builder.CreatePHI(int32Ty, 2);
  payloadIndex->addIncoming(caseIndex, payloadGE4BB);
  payloadIndex->addIncoming(payloadIndex0, payloadLT4BB);

  auto contBB = IGF.createBasicBlock("");
  auto nonZeroSizeBB = IGF.createBasicBlock("");
  auto isNonZero = Builder.CreateICmpNE(truncSize, zero);
  Builder.CreateCondBr(isNonZero, nonZeroSizeBB, contBB);
  {
    Builder.emitBlock(nonZeroSizeBB);
    auto *truncSize = Builder.CreateZExtOrTrunc(size, IGM.Int32Ty);
    auto sizeGTE4 = Builder.CreateICmpUGE(truncSize, four);
    auto sizeClampedTo4 = Builder.CreateSelect(sizeGTE4, four, truncSize);
    // Zero out the payload.
    Builder.CreateMemSet(addr, llvm::ConstantInt::get(IGF.IGM.Int8Ty, 0),
                         truncSize);
    // Store tag into the payload.
    emitStore1to4Bytes(IGF, addr, payloadIndex, sizeClampedTo4);
    Builder.CreateBr(contBB);
  }
  Builder.emitBlock(contBB);

  // Write to the extra tag bytes, if any.
  emitSetTag(IGF, extraTagBitsAddr, extraTagIndex, numExtraTagBytes);
  Builder.CreateBr(returnBB);

  Builder.emitBlock(returnBB);
}

static llvm::Value *projectOutlineBuffer(IRGenFunction &IGF, Address buffer,
                                         llvm::Value *alignmentMask) {
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;
  Address boxAddress(Builder.CreateBitCast(buffer.getAddress(),
                                           IGM.RefCountedPtrTy->getPointerTo()),
                     IGM.RefCountedPtrTy, buffer.getAlignment());
  auto *boxStart = Builder.CreateLoad(boxAddress);
  auto *heapHeaderSize =
      llvm::ConstantInt::get(IGM.SizeTy, IGM.RefCountedStructSize.getValue());
  auto *startOffset =
      Builder.CreateAnd(Builder.CreateAdd(heapHeaderSize, alignmentMask),
                        Builder.CreateNot(alignmentMask));
  auto *addressInBox =
      IGF.emitByteOffsetGEP(boxStart, startOffset, IGM.OpaqueTy);

  addressInBox = Builder.CreateBitCast(addressInBox, IGM.OpaquePtrTy);
  return addressInBox;
}

llvm::Value *TypeLayoutEntry::initBufferWithCopyOfBuffer(IRGenFunction &IGF,
                                                         Address dest,
                                                         Address src) const {
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;

  auto size = this->size(IGF);
  auto alignMask = this->alignmentMask(IGF);
  auto isBitwiseTakable = this->isBitwiseTakable(IGF);

  auto bufferSize = IGM.getSize(getFixedBufferSize(IGM));
  auto bufferAlign = IGM.getSize(Size(getFixedBufferAlignment(IGM).getValue()));
  auto bufferAlignMask = Builder.CreateSub(bufferAlign, IGM.getSize(Size(1)));

  auto bufferAlignFits = Builder.CreateICmpUGE(bufferAlignMask, alignMask);
  auto bufferFits = Builder.CreateICmpUGE(bufferSize, size);

  auto canUseInline = Builder.CreateAnd(isBitwiseTakable, bufferFits);
  canUseInline = Builder.CreateAnd(canUseInline, bufferAlignFits);

  auto inlineBB = IGF.createBasicBlock("inlineBB");
  auto allocateBB = IGF.createBasicBlock("allocateBB");
  auto finishedBB = IGF.createBasicBlock("");
  auto pointerToObject = llvm::PHINode::Create(IGM.OpaquePtrTy, 2);
  Builder.CreateCondBr(canUseInline, inlineBB, allocateBB);

  Builder.emitBlock(inlineBB);
  {
    // Inline of the buffer.
    this->initWithCopy(IGF, dest, src);
    pointerToObject->addIncoming(
        Builder.CreateElementBitCast(dest, IGM.OpaqueTy).getAddress(),
        Builder.GetInsertBlock());
    Builder.CreateBr(finishedBB);
  }

  Builder.emitBlock(allocateBB);
  {
    // The buffer stores a reference to a copy-on-write managed heap buffer.
    auto *destReferenceAddr = Builder.CreateBitCast(
        dest.getAddress(), IGM.RefCountedPtrTy->getPointerTo());
    auto *srcReferenceAddr = Builder.CreateBitCast(
        src.getAddress(), IGM.RefCountedPtrTy->getPointerTo());
    auto *srcReference = Builder.CreateLoad(
        Address(srcReferenceAddr, IGM.RefCountedPtrTy, src.getAlignment()));
    IGF.emitNativeStrongRetain(srcReference, IGF.getDefaultAtomicity());
    Builder.CreateStore(
        srcReference,
        Address(destReferenceAddr, IGM.RefCountedPtrTy, dest.getAlignment()));

    pointerToObject->addIncoming(projectOutlineBuffer(IGF, dest, alignMask),
                                 Builder.GetInsertBlock());
    Builder.CreateBr(finishedBB);
  }

  Builder.emitBlock(finishedBB);
  Builder.Insert(pointerToObject);

  return pointerToObject;
}

void ScalarTypeLayoutEntry::computeProperties() {
  // does not add anything.
}

void ScalarTypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id) const {
  ScalarTypeLayoutEntry::Profile(id, typeInfo, representative);
}

void ScalarTypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id,
                                    const TypeInfo &ti,
                                    SILType ty) {
  id.AddPointer(&ti);
  id.AddPointer(ty.getASTType().getPointer());
}

ScalarTypeLayoutEntry::~ScalarTypeLayoutEntry() {}

llvm::Value *ScalarTypeLayoutEntry::alignmentMask(IRGenFunction  &IGF) const {
  return typeInfo.getAlignmentMask(IGF, representative);
}

llvm::Value *ScalarTypeLayoutEntry::size(IRGenFunction &IGF) const {
  return typeInfo.getSize(IGF, representative);
}

bool ScalarTypeLayoutEntry::isFixedSize(IRGenModule &IGM) const { return true; }

bool ScalarTypeLayoutEntry::isTriviallyDestroyable() const {
  return scalarKind == ScalarKind::TriviallyDestroyable;
}

bool ScalarTypeLayoutEntry::canValueWitnessExtraInhabitantsUpTo(
    IRGenModule &IGM, unsigned index) const {
  return typeInfo.canValueWitnessExtraInhabitantsUpTo(IGM, index);
}

bool ScalarTypeLayoutEntry::isSingleRetainablePointer() const {
  return isNullableRefCounted(scalarKind);
}

llvm::Optional<Size> ScalarTypeLayoutEntry::fixedSize(IRGenModule &IGM) const {
  return typeInfo.getFixedSize();
}

llvm::Optional<Alignment>
ScalarTypeLayoutEntry::fixedAlignment(IRGenModule &IGM) const {
  return typeInfo.getFixedAlignment();
}

llvm::Optional<uint32_t>
ScalarTypeLayoutEntry::fixedXICount(IRGenModule &IGM) const {
  return typeInfo.getFixedExtraInhabitantCount(IGM);
}

llvm::Value *
ScalarTypeLayoutEntry::extraInhabitantCount(IRGenFunction &IGF) const {
  auto &IGM = IGF.IGM;
  auto fixedXICount = typeInfo.getFixedExtraInhabitantCount(IGM);
  return llvm::ConstantInt::get(IGM.Int32Ty, fixedXICount);
}

llvm::Value *ScalarTypeLayoutEntry::isBitwiseTakable(IRGenFunction &IGF) const {
  return llvm::ConstantInt::get(
      IGF.IGM.Int1Ty, typeInfo.isBitwiseTakable(ResilienceExpansion::Maximal));
}

llvm::Constant *
ScalarTypeLayoutEntry::layoutString(IRGenModule &IGM,
                                    GenericSignature genericSig) const {
  return nullptr;
}

bool ScalarTypeLayoutEntry::refCountString(IRGenModule &IGM,
                                           LayoutStringBuilder &B,
                                           GenericSignature genericSig) const {
  auto size = typeInfo.getFixedSize().getValue();
  switch (scalarKind) {
  case ScalarKind::ErrorReference:
    B.addRefCount(LayoutStringBuilder::RefCountingKind::Error, size);
    break;
  case ScalarKind::NativeStrongReference:
    B.addRefCount(LayoutStringBuilder::RefCountingKind::NativeStrong, size);
    break;
  case ScalarKind::NativeUnownedReference:
    B.addRefCount(LayoutStringBuilder::RefCountingKind::NativeUnowned, size);
    break;
  case ScalarKind::NativeWeakReference:
    B.addRefCount(LayoutStringBuilder::RefCountingKind::NativeWeak, size);
    break;
  case ScalarKind::UnknownWeakReference:
    B.addRefCount(LayoutStringBuilder::RefCountingKind::UnknownWeak, size);
    break;
  case ScalarKind::UnknownReference:
    B.addRefCount(LayoutStringBuilder::RefCountingKind::Unknown, size);
    break;
  case ScalarKind::UnknownUnownedReference:
    B.addRefCount(LayoutStringBuilder::RefCountingKind::UnknownUnowned, size);
    break;
  case ScalarKind::BridgeReference:
    B.addRefCount(LayoutStringBuilder::RefCountingKind::Bridge, size);
    break;
  case ScalarKind::BlockReference:
    B.addRefCount(LayoutStringBuilder::RefCountingKind::Block, size);
    break;
  case ScalarKind::ObjCReference:
    B.addRefCount(LayoutStringBuilder::RefCountingKind::ObjC, size);
    break;
  case ScalarKind::ThickFunc:
    B.addSkip(IGM.getPointerSize().getValue());
    B.addRefCount(LayoutStringBuilder::RefCountingKind::NativeStrong,
                  IGM.getPointerSize().getValue());
    break;
  case ScalarKind::TriviallyDestroyable:
    B.addSkip(size);
    break;
  case ScalarKind::ExistentialReference:
    B.addRefCount(LayoutStringBuilder::RefCountingKind::Existential, size);
    break;
  case ScalarKind::CustomReference:
    return false;
  default:
    llvm_unreachable("Unsupported ScalarKind");
  }

  return true;
}

void ScalarTypeLayoutEntry::destroy(IRGenFunction &IGF, Address addr) const {
  switch (scalarKind) {
  case ScalarKind::TriviallyDestroyable:
    return;
  case ScalarKind::Immovable:
    llvm_unreachable("cannot opaquely manipulate immovable types!");
  case ScalarKind::NativeStrongReference: {
    auto alignment = typeInfo.getFixedAlignment();
    auto addressType = typeInfo.getStorageType();
    auto *castAddr = IGF.Builder.CreateBitCast(addr.getAddress(),
                                               addressType->getPointerTo());
    addr = Address(castAddr, addressType, alignment);
    llvm::Value *val = IGF.Builder.CreateLoad(addr, "toDestroy");
    IGF.emitNativeStrongRelease(val, IGF.getDefaultAtomicity());
    return;
  }
  case ScalarKind::ErrorReference: {
    auto alignment = typeInfo.getFixedAlignment();
    auto addressType = typeInfo.getStorageType();
    auto *castAddr = IGF.Builder.CreateBitCast(addr.getAddress(),
                                               addressType->getPointerTo());
    addr = Address(castAddr, addressType, alignment);
    llvm::Value *val = IGF.Builder.CreateLoad(addr, "toDestroy");
    IGF.emitErrorStrongRelease(val);
    return;
  }
  case ScalarKind::NativeWeakReference: {
    IGF.emitNativeWeakDestroy(addr);
    return;
  }
  case ScalarKind::NativeUnownedReference: {
    IGF.emitNativeUnownedDestroy(addr);
    return;
  }
  case ScalarKind::BlockReference: {
    IGF.emitBlockRelease(addr.getAddress());
    return;
  }
  case ScalarKind::UnknownReference: {
    auto alignment = typeInfo.getFixedAlignment();
    auto addressType = typeInfo.getStorageType();
    auto *castAddr = IGF.Builder.CreateBitCast(addr.getAddress(),
                                               addressType->getPointerTo());
    addr = Address(castAddr, addressType, alignment);
    llvm::Value *val = IGF.Builder.CreateLoad(addr, "toDestroy");
    IGF.emitUnknownStrongRelease(val, IGF.getDefaultAtomicity());
    return;
  }
  case ScalarKind::UnknownUnownedReference: {
    IGF.emitUnknownUnownedDestroy(addr);
    return;
  }
  case ScalarKind::UnknownWeakReference: {
    IGF.emitUnknownWeakDestroy(addr);
    return;
  }
  case ScalarKind::BridgeReference: {
    auto alignment = typeInfo.getFixedAlignment();
    auto addressType = typeInfo.getStorageType()->getPointerTo();
    auto *castAddr = IGF.Builder.CreateBitCast(addr.getAddress(),
                                               addressType->getPointerTo());
    addr = Address(castAddr, addressType, alignment);
    llvm::Value *val = IGF.Builder.CreateLoad(addr, "toDestroy");
    IGF.emitBridgeStrongRelease(val, IGF.getDefaultAtomicity());
    return;
  }
  case ScalarKind::ObjCReference: {
    auto alignment = typeInfo.getFixedAlignment();
    auto addressType = typeInfo.getStorageType();
    auto *castAddr = IGF.Builder.CreateBitCast(addr.getAddress(),
                                               addressType->getPointerTo());
    addr = Address(castAddr, addressType, alignment);
    llvm::Value *val = IGF.Builder.CreateLoad(addr, "toDestroy");
    IGF.emitObjCStrongRelease(val);
    break;
  }
  case ScalarKind::BlockStorage: {
    // The frontend will currently never emit copy_addr or destroy_addr for
    // block storage.
    IGF.unimplemented(SourceLoc(), "destroying @block_storage");
    return;
  }
  case ScalarKind::ThickFunc: {
    // A thick function is a pair of a function pointer and an optional
    // ref counted opaque pointer. We don't need to do anything for the
    // function pointer, but we need to release the opaque pointer.
    auto alignment = typeInfo.getFixedAlignment();
    auto addressType = typeInfo.getStorageType();
    auto *castAddr = IGF.Builder.CreateBitCast(addr.getAddress(),
                                               addressType->getPointerTo());
    addr = Address(castAddr, addressType, alignment);
    auto secondElement = IGF.Builder.CreateStructGEP(
        addr, 1, IGF.IGM.getPointerSize(),
        addr->getName() + ".data");
    auto pointer = IGF.Builder.CreateLoad(secondElement, "toDestroy");
    IGF.emitNativeStrongRelease(pointer, IGF.getDefaultAtomicity());
    return;
  }
  case ScalarKind::ExistentialReference: {
    emitDestroyBoxedOpaqueExistentialBuffer(IGF, representative, addr);
    return;
  }
  case ScalarKind::CustomReference: {
    typeInfo.destroy(IGF, addr, representative, true);
  }
  }
}

void ScalarTypeLayoutEntry::assignWithCopy(IRGenFunction &IGF, Address dest,
                                           Address src) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto storageTy = typeInfo.getStorageType();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto &Builder = IGF.Builder;
  dest = Address(Builder.CreateBitCast(dest.getAddress(), addressType),
                 storageTy, alignment);
  src = Address(Builder.CreateBitCast(src.getAddress(), addressType), storageTy,
                alignment);
  typeInfo.assignWithCopy(IGF, dest, src, representative, true);
}

void ScalarTypeLayoutEntry::assignWithTake(IRGenFunction &IGF, Address dest,
                                           Address src) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto storageTy = typeInfo.getStorageType();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto &Builder = IGF.Builder;
  dest = Address(Builder.CreateBitCast(dest.getAddress(), addressType),
                 storageTy, alignment);
  src = Address(Builder.CreateBitCast(src.getAddress(), addressType), storageTy,
                alignment);
  typeInfo.assignWithTake(IGF, dest, src, representative, true);
}

void ScalarTypeLayoutEntry::initWithCopy(IRGenFunction &IGF, Address dest,
                                         Address src) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto storageTy = typeInfo.getStorageType();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto &Builder = IGF.Builder;
  dest = Address(Builder.CreateBitCast(dest.getAddress(), addressType),
                 storageTy, alignment);
  src = Address(Builder.CreateBitCast(src.getAddress(), addressType), storageTy,
                alignment);
  typeInfo.initializeWithCopy(IGF, dest, src, representative, true);
}

void ScalarTypeLayoutEntry::initWithTake(IRGenFunction &IGF, Address dest,
                                         Address src) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto storageTy = typeInfo.getStorageType();
  auto &Builder = IGF.Builder;
  dest = Address(Builder.CreateBitCast(dest.getAddress(), addressType),
                 storageTy, alignment);
  src = Address(Builder.CreateBitCast(src.getAddress(), addressType), storageTy,
                alignment);
  typeInfo.initializeWithTake(IGF, dest, src, representative, true);
}

llvm::Value *ScalarTypeLayoutEntry::getEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *numEmptyCases, Address value) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto storageTy = typeInfo.getStorageType();
  auto &Builder = IGF.Builder;
  value = Address(Builder.CreateBitCast(value.getAddress(), addressType),
                  storageTy, alignment);

  return typeInfo.getEnumTagSinglePayload(IGF, numEmptyCases, value,
                                          representative, true);
}

void ScalarTypeLayoutEntry::storeEnumTagSinglePayload(IRGenFunction &IGF,
                                                llvm::Value *tag,
                                                llvm::Value *numEmptyCases,
                                                Address addr) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto storageTy = typeInfo.getStorageType();
  auto addressType = typeInfo.getStorageType()->getPointerTo();
  auto &Builder = IGF.Builder;
  addr = Address(Builder.CreateBitCast(addr.getAddress(), addressType),
                 storageTy, alignment);
  typeInfo.storeEnumTagSinglePayload(IGF, tag, numEmptyCases, addr,
                                     representative, true);
}

bool ScalarTypeLayoutEntry::classof(const TypeLayoutEntry *entry) {
  return entry->kind == TypeLayoutEntryKind::Scalar;
}

llvm::Optional<const FixedTypeInfo *>
ScalarTypeLayoutEntry::getFixedTypeInfo() const {
  return &typeInfo;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void ScalarTypeLayoutEntry::dump() const {
  if (typeInfo.isFixedSize())
    llvm::dbgs() << "{ scalar isFixedSize:" << (bool)typeInfo.isFixedSize()
                 << " scalarKind: " << scalarToString(scalarKind)
                 << " isLoadable: " << (bool)typeInfo.isLoadable() << " size: "
                 << cast<FixedTypeInfo>(typeInfo).getFixedSize().getValue()
                 << " alignment: "
                 << cast<FixedTypeInfo>(typeInfo).getFixedAlignment().getValue()
                 << " rep: " << representative << " id: " << this << " }\n";
  if (!typeInfo.isFixedSize()) {
    llvm::dbgs() << "{ scalar non-fixed rep: " << representative
                 << " id: " << this << " }\n";
  }
}
#endif

void AlignedGroupEntry::computeProperties() {
  for (auto *entry : entries) {
    gatherProperties(entry);
  }
}

void AlignedGroupEntry::Profile(llvm::FoldingSetNodeID &id) const {
  AlignedGroupEntry::Profile(id, entries, minimumAlignment);
}

void AlignedGroupEntry::Profile(llvm::FoldingSetNodeID &id,
                                const std::vector<TypeLayoutEntry *> &entries,
                                Alignment::int_type minimumAlignment) {
  for (auto *entry : entries)
    id.AddPointer(entry);
  id.AddInteger(minimumAlignment);
}

AlignedGroupEntry::~AlignedGroupEntry() {}

llvm::Value *AlignedGroupEntry::alignmentMask(IRGenFunction &IGF) const {
  auto &IGM = IGF.IGM;
  auto minimumAlignmentMask = IGM.getSize(Size(minimumAlignment - 1));
  if (isFixedSize(IGF.IGM))
    return minimumAlignmentMask;

  auto &Builder = IGF.Builder;
  llvm::Value *currentMaxAlignment = minimumAlignmentMask;
  for(auto *entry : entries) {
    auto entryAlignmentMask = entry->alignmentMask(IGF);
    currentMaxAlignment =
        Builder.CreateOr(entryAlignmentMask, currentMaxAlignment);
  }
  currentMaxAlignment->setName("alignment-mask");
  return currentMaxAlignment;
}

llvm::Value *AlignedGroupEntry::size(IRGenFunction &IGF) const {
  llvm::Value *currentSize = nullptr;
  auto &Builder = IGF.Builder;
  for(auto *entry : entries) {
    if (!currentSize) {
      currentSize = entry->size(IGF);
      continue;
    }
    // alignupto(currentSize, entry.alignment) + entry.size
    auto entryAlignMask = entry->alignmentMask(IGF);
    auto invertedMask = Builder.CreateNot(entryAlignMask);
    currentSize = Builder.CreateAdd(currentSize, entryAlignMask);
    currentSize = Builder.CreateAnd(currentSize, invertedMask);
    currentSize = Builder.CreateAdd(currentSize, entry->size(IGF));
  }
  currentSize->setName("size");
  return currentSize;
}

bool AlignedGroupEntry::isFixedSize(IRGenModule &IGM) const {
  return fixedSize(IGM).has_value();
}

bool AlignedGroupEntry::isTriviallyDestroyable() const {
  for (auto *entry : entries) {
    if (!entry->isTriviallyDestroyable()) {
      return false;
    }
  }
  return true;
}

bool AlignedGroupEntry::canValueWitnessExtraInhabitantsUpTo(
    IRGenModule &IGM, unsigned index) const {
  uint32_t currentMaxXICount = 0;
  uint32_t currentMaxXIField = 0;
  // Choose the field with the max xi count.
  for (unsigned i = 0; i < entries.size(); i++) {
    auto entryXICount = entries[i]->fixedXICount(IGM);
    if (!entryXICount) {
      return false;
    }
    if (*entryXICount > currentMaxXICount) {
      currentMaxXICount = *entryXICount;
      currentMaxXIField = i;
    }
  }

  // The non-extra-inhabitant-providing fields of the type must be
  // trivial, because an enum may contain garbage values in those fields'
  // storage which the value witness operation won't handle.
  for (unsigned i = 0; i < entries.size(); i++) {
    if (i == currentMaxXIField)
      continue;
    if (!entries[i]->isTriviallyDestroyable()) {
      return false;
    }
  }

  return entries[currentMaxXIField]->canValueWitnessExtraInhabitantsUpTo(IGM,
                                                                         index);
}

bool AlignedGroupEntry::isSingleRetainablePointer() const { return false; }

llvm::Optional<Size> AlignedGroupEntry::fixedSize(IRGenModule &IGM) const {
  if (_fixedSize.has_value())
    return *_fixedSize;
  Size currentSize(0);
  for (auto *entry : entries) {
    if (!entry->fixedSize(IGM) || !entry->fixedAlignment(IGM)) {
      return *(_fixedSize = llvm::Optional<Size>(llvm::None));
    }
    Size entrySize = *entry->fixedSize(IGM);
    currentSize =
        currentSize.roundUpToAlignment(*entry->fixedAlignment(IGM)) + entrySize;
  }
  return *(_fixedSize = currentSize);
}

llvm::Optional<Alignment>
AlignedGroupEntry::fixedAlignment(IRGenModule &IGM) const {
  if (_fixedAlignment.has_value())
    return *_fixedAlignment;

  Alignment currentAlignment = Alignment(
    std::max((Alignment::int_type)1, minimumAlignment));
  for (auto *entry : entries) {
    if (!entry->fixedAlignment(IGM)) {
      return *(_fixedAlignment = llvm::Optional<Alignment>(llvm::None));
    }
    currentAlignment = std::max(currentAlignment, *entry->fixedAlignment(IGM));
  }
  return *(_fixedAlignment = currentAlignment);
}

llvm::Optional<uint32_t>
AlignedGroupEntry::fixedXICount(IRGenModule &IGM) const {
  if (_fixedXICount.has_value())
    return *_fixedXICount;
  uint32_t currentMaxXICount = 0;
  // Choose the field with the max xi count.
  for (auto *entry : entries) {
    auto entryXICount = entry->fixedXICount(IGM);
    if (!entryXICount) {
      return *(_fixedXICount = llvm::Optional<uint32_t>(llvm::None));
    }
    currentMaxXICount = std::max(*entryXICount, currentMaxXICount);
  }
  return *(_fixedXICount = currentMaxXICount);
}

llvm::Value *AlignedGroupEntry::extraInhabitantCount(IRGenFunction &IGF) const {
  llvm::Value *currentMaxXICount = IGF.IGM.getInt32(0);
  auto &Builder = IGF.Builder;
  // Choose the field with the max xi count.
  for (auto *entry : entries) {
    auto entryXICount = entry->extraInhabitantCount(IGF);
    auto entryXICountGT =
        Builder.CreateICmpUGT(entryXICount, currentMaxXICount);
    currentMaxXICount =
        Builder.CreateSelect(entryXICountGT, entryXICount, currentMaxXICount);
  }
  currentMaxXICount->setName("num-extra-inhabitants");
  return currentMaxXICount;
}

llvm::Value *AlignedGroupEntry::isBitwiseTakable(IRGenFunction &IGF) const {
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;
  llvm::Value *isBitwiseTakable = llvm::ConstantInt::get(IGM.Int1Ty, true);
  for(auto *entry : entries) {
    isBitwiseTakable =
        Builder.CreateAnd(isBitwiseTakable, entry->isBitwiseTakable(IGF));
  }
  return isBitwiseTakable;
}

llvm::Constant *
AlignedGroupEntry::layoutString(IRGenModule &IGM,
                                GenericSignature genericSig) const {
  if (_layoutString) {
    return *_layoutString;
  }

  LayoutStringBuilder B{};

  if (!refCountString(IGM, B, genericSig)) {
    return *(_layoutString = llvm::Optional<llvm::Constant *>(nullptr));
  }

  ConstantInitBuilder IB(IGM);
  auto SB = IB.beginStruct();
  SB.setPacked(true);

  B.result(IGM, SB, genericSig);

  IRGenMangler mangler;
  std::string symbolName =
      mangler.mangleSymbolNameForMangledMetadataAccessorString(
          "type_layout_string", genericSig.getCanonicalSignature(),
          ty.getASTType()->mapTypeOutOfContext()->getCanonicalType());

  auto *global = SB.finishAndCreateGlobal(symbolName, IGM.getPointerAlignment(),
                                          /*constant*/ true);
  IGM.setTrueConstGlobal(global);
  _layoutString = global;

  return global;
}

bool AlignedGroupEntry::refCountString(IRGenModule &IGM, LayoutStringBuilder &B,
                                       GenericSignature genericSig) const {
  if (!isFixedSize(IGM)) {
    return false;
  }

  uint64_t offset = 0;
  for (auto *entry : entries) {
    if (offset) {
      uint64_t alignmentMask = entry->fixedAlignment(IGM)->getMaskValue();
      uint64_t alignedOffset = offset + alignmentMask;
      alignedOffset &= ~alignmentMask;
      if (alignedOffset > offset) {
        B.addSkip(alignedOffset - offset);
        offset = alignedOffset;
      }
    }
    if (!entry->refCountString(IGM, B, genericSig)) {
      return false;
    }
    offset += entry->fixedSize(IGM)->getValue();
  }

  return true;
}

static Address alignAddress(IRGenFunction &IGF, Address addr,
                            llvm::Value *alignMask) {
  auto &Builder = IGF.Builder;
  auto &IGM = IGF.IGM;
  auto ptr = Builder.CreatePtrToInt(addr.getAddress(), IGM.SizeTy);
  ptr = Builder.CreateAdd(ptr, alignMask);
  ptr = Builder.CreateAnd(ptr, Builder.CreateNot(alignMask));
  ptr = Builder.CreateIntToPtr(ptr, IGM.OpaquePtrTy);
  return Address(ptr, IGM.OpaqueTy, Alignment(1));
}

static Address addOffset(IRGenFunction &IGF, Address addr,
                         llvm::Value *offset) {
  auto &Builder = IGF.Builder;
  auto &IGM = IGF.IGM;
  auto ptr = Builder.CreatePtrToInt(addr.getAddress(), IGM.SizeTy);
  ptr = Builder.CreateAdd(ptr, offset);
  ptr = Builder.CreateIntToPtr(ptr, IGM.OpaquePtrTy);
  return Address(ptr, IGM.OpaqueTy, Alignment(1));
}

void AlignedGroupEntry::destroy(IRGenFunction &IGF, Address addr) const {
  if (isFixedSize(IGF.IGM)) {
    Size offset(0);
    for (auto entry : entries) {
      uint64_t aligned = offset.getValue();
      // Align the offset
      aligned += entry->fixedAlignment(IGF.IGM)->getMaskValue();
      aligned &= ~(entry->fixedAlignment(IGF.IGM)->getMaskValue());
      offset = Size(aligned);
      auto projected = IGF.emitByteOffsetGEP(
          addr.getAddress(),
          llvm::ConstantInt::get(IGF.IGM.Int32Ty, offset.getValue()),
          IGF.IGM.OpaquePtrTy);
      entry->destroy(IGF, Address(projected, IGF.IGM.OpaquePtrTy,
                                  *entry->fixedAlignment(IGF.IGM)));
      offset += *entry->fixedSize(IGF.IGM);
    }
  } else {
    Address currentDest = addr;
    auto remainingEntries = entries.size();
    for (auto *entry : entries) {
      if (currentDest.getAddress() != addr.getAddress()) {
        // Align upto the current entry's requirement.
        auto entryMask = entry->alignmentMask(IGF);
        currentDest = alignAddress(IGF, currentDest, entryMask);
      }

      entry->destroy(IGF, currentDest);

      --remainingEntries;
      if (remainingEntries == 0)
        continue;

      auto entrySize = entry->size(IGF);
      currentDest = addOffset(IGF, currentDest, entrySize);
    }
  }
}

void AlignedGroupEntry::withEachEntry(
    IRGenFunction &IGF, Address dest, Address src,
    llvm::function_ref<void(TypeLayoutEntry *entry, Address entryDest,
                            Address entrySrc)>
        entryFun) const {
  if (isFixedSize(IGF.IGM)) {
    Size offset(0);
    for (auto entry : entries) {
      uint64_t aligned = offset.getValue();
      // Align the offset
      aligned += entry->fixedAlignment(IGF.IGM)->getMaskValue();
      aligned &= ~(entry->fixedAlignment(IGF.IGM)->getMaskValue());
      offset = Size(aligned);
      auto projectedSrc = IGF.emitByteOffsetGEP(
          src.getAddress(),
          llvm::ConstantInt::get(IGF.IGM.Int32Ty, offset.getValue()),
          IGF.IGM.OpaquePtrTy);
      auto projectedDest = IGF.emitByteOffsetGEP(
          dest.getAddress(),
          llvm::ConstantInt::get(IGF.IGM.Int32Ty, offset.getValue()),
          IGF.IGM.OpaquePtrTy);
      entryFun(entry,
               Address(projectedDest, IGF.IGM.OpaquePtrTy,
                       *entry->fixedAlignment(IGF.IGM)),
               Address(projectedSrc, IGF.IGM.OpaquePtrTy,
                       *entry->fixedAlignment(IGF.IGM)));
      offset += *entry->fixedSize(IGF.IGM);
    }
  } else {
    Address currentDest = dest;
    Address currentSrc = src;
    auto remainingEntries = entries.size();
    for (auto *entry : entries) {
      if (currentDest.getAddress() != dest.getAddress()) {
        // Align upto the current entry's requirement.
        auto entryMask = entry->alignmentMask(IGF);
        currentDest = alignAddress(IGF, currentDest, entryMask);
        currentSrc = alignAddress(IGF, currentSrc, entryMask);
      }

      entryFun(entry, currentDest, currentSrc);

      --remainingEntries;
      if (remainingEntries == 0)
        continue;

      auto entrySize = entry->size(IGF);
      currentDest = addOffset(IGF, currentDest, entrySize);
      currentSrc = addOffset(IGF, currentSrc, entrySize);
    }
  }
}

void AlignedGroupEntry::assignWithCopy(IRGenFunction &IGF, Address dest,
                                       Address src) const {
  withEachEntry(
      IGF, dest, src,
      [&](TypeLayoutEntry *entry, Address entryDest, Address entrySrc) {
        entry->assignWithCopy(IGF, entryDest, entrySrc);
      });
}

void AlignedGroupEntry::assignWithTake(IRGenFunction &IGF, Address dest,
                                       Address src) const {
  withEachEntry(
      IGF, dest, src,
      [&](TypeLayoutEntry *entry, Address entryDest, Address entrySrc) {
        entry->assignWithTake(IGF, entryDest, entrySrc);
      });
}

void AlignedGroupEntry::initWithCopy(IRGenFunction &IGF, Address dest,
                                     Address src) const {
  withEachEntry(
      IGF, dest, src,
      [&](TypeLayoutEntry *entry, Address entryDest, Address entrySrc) {
        entry->initWithCopy(IGF, entryDest, entrySrc);
      });
}

void AlignedGroupEntry::initWithTake(IRGenFunction &IGF, Address dest,
                                     Address src) const {
  withEachEntry(
      IGF, dest, src,
      [&](TypeLayoutEntry *entry, Address entryDest, Address entrySrc) {
        entry->initWithTake(IGF, entryDest, entrySrc);
      });
}

llvm::Value *AlignedGroupEntry::withExtraInhabitantProvidingEntry(
    IRGenFunction &IGF, Address addr, llvm::Type *returnType,
    llvm::function_ref<llvm::Value *(TypeLayoutEntry *, Address, llvm::Value *)>
        entryFun) const {
  if (isFixedSize(IGF.IGM)) {
    // First compute the max xi count.
    auto maxXICount = fixedXICount(IGF.IGM);
    Size offset(0);
    for (auto entry : entries) {
      uint64_t aligned = offset.getValue();
      // Align the offset
      aligned += entry->fixedAlignment(IGF.IGM)->getMaskValue();
      aligned &= ~(entry->fixedAlignment(IGF.IGM)->getMaskValue());
      offset = Size(aligned);

      auto xiCount = entry->fixedXICount(IGF.IGM);
      if (xiCount == maxXICount) {
        auto projected = IGF.emitByteOffsetGEP(
            addr.getAddress(),
            llvm::ConstantInt::get(IGF.IGM.Int32Ty, offset.getValue()),
            IGF.IGM.OpaquePtrTy);
        return entryFun(entry,
                        Address(projected, IGF.IGM.OpaquePtrTy,
                                *entry->fixedAlignment(IGF.IGM)),
                        llvm::ConstantInt::get(IGF.IGM.Int32Ty, *xiCount));
      }
      offset += *entry->fixedSize(IGF.IGM);
    }
    llvm_unreachable("Failed to find entry that contains the xis");
  } else {
    // First compute the max xi count.
    auto maxXICount = extraInhabitantCount(IGF);

    Address currentAddr = addr;
    auto &IGM = IGF.IGM;
    auto &Builder = IGF.Builder;
    auto remainingEntries = entries.size();
    // Select the first field that matches the max xi count.
    auto mergeBB = IGF.createBasicBlock("found_max_xi");
    llvm::PHINode *mergePHI = nullptr;
    if (returnType != IGM.VoidTy)
      mergePHI = llvm::PHINode::Create(IGM.Int32Ty, remainingEntries);
    for (auto *entry : entries) {
      if (currentAddr.getAddress() != addr.getAddress()) {
        // Align upto the current entry's requirement.
        auto entryMask = entry->alignmentMask(IGF);
        currentAddr = alignAddress(IGF, currentAddr, entryMask);
      }
      auto xiCount = entry->extraInhabitantCount(IGF);

      auto isMaxXICount = Builder.CreateICmpEQ(xiCount, maxXICount);
      auto trueBB = IGF.createBasicBlock("");
      auto falseBB = IGF.createBasicBlock("");

      Builder.CreateCondBr(isMaxXICount, trueBB, falseBB);
      ConditionalDominanceScope scope(IGF);
      Builder.emitBlock(trueBB);

      auto tag = entryFun(entry, currentAddr, xiCount);
      if (mergePHI)
        mergePHI->addIncoming(tag, Builder.GetInsertBlock());
      Builder.CreateBr(mergeBB);

      Builder.emitBlock(falseBB);
      --remainingEntries;
      if (remainingEntries != 0) {
        auto entrySize = entry->size(IGF);
        currentAddr = addOffset(IGF, currentAddr, entrySize);
      }
    }
    // We should have found an entry with max xi count.
    Builder.CreateUnreachable();
    Builder.emitBlock(mergeBB);
    if (mergePHI)
      Builder.Insert(mergePHI);
    return mergePHI;
  }
}

llvm::Value *AlignedGroupEntry::getEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *numEmptyCases, Address addr) const {
  return getEnumTagSinglePayloadGeneric(
      IGF, addr, numEmptyCases, [&](Address addr) -> llvm::Value * {
        // Get tag in payload.
        auto tagInPayload = withExtraInhabitantProvidingEntry(
            IGF, addr, IGF.IGM.Int32Ty,
            [&](TypeLayoutEntry *entry, Address entryAddress,
                llvm::Value *entryXICount) -> llvm::Value * {
              return entry->getEnumTagSinglePayload(IGF, entryXICount,
                                                    entryAddress);
            });
        return tagInPayload;
      });
}

void AlignedGroupEntry::storeEnumTagSinglePayload(IRGenFunction &IGF,
                                                  llvm::Value *tag,
                                                  llvm::Value *numEmptyCases,
                                                  Address addr) const {
  storeEnumTagSinglePayloadGeneric(
      IGF, tag, numEmptyCases, addr, [&](Address addr, llvm::Value *tag) {
        // Store the extra inhabitant.
        withExtraInhabitantProvidingEntry(
            IGF, addr, IGF.IGM.VoidTy,
            [&](TypeLayoutEntry *entry, Address entryAddress,
                llvm::Value *entryXICount) -> llvm::Value * {
              entry->storeEnumTagSinglePayload(IGF, tag, entryXICount,
                                               entryAddress);
              return nullptr;
            });
      });
}

bool AlignedGroupEntry::classof(const TypeLayoutEntry *entry) {
  return entry->kind == TypeLayoutEntryKind::AlignedGroup;
}

llvm::Optional<const FixedTypeInfo *>
AlignedGroupEntry::getFixedTypeInfo() const {
  return fixedTypeInfo;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void AlignedGroupEntry::dump() const {
    llvm::dbgs() << "{ aligned group:\n";
    llvm::dbgs() << "  alignment: " << minimumAlignment << "\n";
    for (auto *entry : entries) {
      entry->dump();
    }
    llvm::dbgs() << "  id: " << this  << "}\n";
}
#endif

void ArchetypeLayoutEntry::computeProperties() { hasArchetypeField = true; }

void ArchetypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id) const {
  ArchetypeLayoutEntry::Profile(id, archetype);
}

void ArchetypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id,
                                   SILType archetype) {
  id.AddPointer(archetype.getASTType().getPointer());
}

ArchetypeLayoutEntry::~ArchetypeLayoutEntry() {}

llvm::Value *ArchetypeLayoutEntry::alignmentMask(IRGenFunction &IGF) const {
  return emitLoadOfAlignmentMask(IGF, archetype);
}

llvm::Value *ArchetypeLayoutEntry::size(IRGenFunction &IGF) const {
  return emitLoadOfSize(IGF, archetype);
}

bool ArchetypeLayoutEntry::isFixedSize(IRGenModule &IGM) const { return false; }

bool ArchetypeLayoutEntry::isTriviallyDestroyable() const { return false; }

bool ArchetypeLayoutEntry::canValueWitnessExtraInhabitantsUpTo(
    IRGenModule &IGM, unsigned index) const {
  return false;
}

bool ArchetypeLayoutEntry::isSingleRetainablePointer() const { return false; }

llvm::Optional<Size> ArchetypeLayoutEntry::fixedSize(IRGenModule &IGM) const {
  return llvm::None;
}

llvm::Optional<Alignment>
ArchetypeLayoutEntry::fixedAlignment(IRGenModule &IGM) const {
  return llvm::None;
}

llvm::Optional<uint32_t>
ArchetypeLayoutEntry::fixedXICount(IRGenModule &IGM) const {
  return llvm::None;
}

llvm::Value *
ArchetypeLayoutEntry::extraInhabitantCount(IRGenFunction &IGF) const {
  return emitLoadOfExtraInhabitantCount(IGF, archetype);
}

llvm::Value *
ArchetypeLayoutEntry::isBitwiseTakable(IRGenFunction &IGF) const {
  return emitLoadOfIsBitwiseTakable(IGF, archetype);
}

llvm::Constant *
ArchetypeLayoutEntry::layoutString(IRGenModule &IGM,
                                   GenericSignature genericSig) const {
  return nullptr;
}

bool ArchetypeLayoutEntry::refCountString(IRGenModule &IGM,
                                          LayoutStringBuilder &B,
                                          GenericSignature genericSig) const {
  return false;
}

void ArchetypeLayoutEntry::destroy(IRGenFunction &IGF, Address addr) const {
  emitDestroyCall(IGF, archetype, addr);
}

void ArchetypeLayoutEntry::assignWithCopy(IRGenFunction &IGF, Address dest,
                                          Address src) const {
  emitAssignWithCopyCall(IGF, archetype, dest, src);
}

void ArchetypeLayoutEntry::assignWithTake(IRGenFunction &IGF, Address dest,
                                          Address src) const {
  emitAssignWithTakeCall(IGF, archetype, dest, src);
}

void ArchetypeLayoutEntry::initWithCopy(IRGenFunction &IGF, Address dest,
                                        Address src) const {
  emitInitializeWithCopyCall(IGF, archetype, dest, src);
}

void ArchetypeLayoutEntry::initWithTake(IRGenFunction &IGF, Address dest,
                                        Address src) const {
  emitInitializeWithTakeCall(IGF, archetype, dest, src);
}

llvm::Value *ArchetypeLayoutEntry::getEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *numEmptyCases, Address value) const {
  value = Address(
      IGF.Builder.CreateBitCast(value.getAddress(), IGF.IGM.OpaquePtrTy),
      IGF.IGM.OpaqueTy, value.getAlignment());

  return emitGetEnumTagSinglePayloadCall(IGF, archetype, numEmptyCases, value);
}

void ArchetypeLayoutEntry::storeEnumTagSinglePayload(IRGenFunction &IGF,
                                                     llvm::Value *tag,
                                                     llvm::Value *numEmptyCases,
                                                     Address addr) const {
  addr =
      Address(IGF.Builder.CreateBitCast(addr.getAddress(), IGF.IGM.OpaquePtrTy),
              IGF.IGM.OpaqueTy, addr.getAlignment());

  emitStoreEnumTagSinglePayloadCall(IGF, archetype, tag, numEmptyCases, addr);
}

bool ArchetypeLayoutEntry::classof(const TypeLayoutEntry *entry) {
  return entry->kind == TypeLayoutEntryKind::Archetype;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void ArchetypeLayoutEntry::dump() const {
  llvm::dbgs() << "{ archetype: " << archetype << " id: " << this << " }\n";
}
#endif

void EnumTypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id) const {
  EnumTypeLayoutEntry::Profile(id, numEmptyCases, cases);
}

void EnumTypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id,
                                  unsigned numEmptyCases,
                                  const std::vector<TypeLayoutEntry *> &cases) {
  id.AddInteger(numEmptyCases);
  for (auto *layout : cases)
    id.AddPointer(layout);
}

EnumTypeLayoutEntry::~EnumTypeLayoutEntry() {}

llvm::Value *EnumTypeLayoutEntry::alignmentMask(IRGenFunction &IGF) const {
  assert(!cases.empty());
  auto &IGM = IGF.IGM;
  auto minimumAlignmentMask = IGM.getSize(Size(minimumAlignment - 1));

  auto &Builder = IGF.Builder;
  llvm::Value *currentMaxAlignment = minimumAlignmentMask;
  for(auto *entry : cases) {
    auto entryAlignmentMask = entry->alignmentMask(IGF);
    currentMaxAlignment =
        Builder.CreateOr(entryAlignmentMask, currentMaxAlignment);
  }
  return currentMaxAlignment;
}

llvm::Value *EnumTypeLayoutEntry::maxPayloadSize(IRGenFunction &IGF) const {
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;
  llvm::Value *payloadSize = IGM.getSize(Size(0));
  for (auto &entry: cases) {
    auto entrySize = entry->size(IGF);
    auto gt = Builder.CreateICmpUGT(entrySize, payloadSize);
    payloadSize = Builder.CreateSelect(gt, entrySize, payloadSize);
    payloadSize->setName("payload-size");
  }
  return payloadSize;
}

llvm::Value *EnumTypeLayoutEntry::isBitwiseTakable(IRGenFunction &IGF) const {
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;
  llvm::Value *isBitwiseTakable = llvm::ConstantInt::get(IGM.Int1Ty, true);
  for (auto &entry: cases) {
    isBitwiseTakable =
        Builder.CreateAnd(isBitwiseTakable, entry->isBitwiseTakable(IGF));
  }
  return isBitwiseTakable;
}

bool EnumTypeLayoutEntry::buildSinglePayloadRefCountString(
    IRGenModule &IGM, LayoutStringBuilder &B,
    GenericSignature genericSig) const {
  if (!isFixedSize(IGM))
    return false;

  unsigned xiTagValues = 0;
  unsigned extraTagByteCount = 0;
  uint64_t zeroTagValue = 0;
  unsigned xiBitCount = 0;
  unsigned xiBitOffset = 0;
  bool isSimple = true;

  auto &payloadTI = **cases[0]->getFixedTypeInfo();

  if (payloadTI.mayHaveExtraInhabitants(IGM)) {
    xiTagValues =
        std::min(numEmptyCases, payloadTI.getFixedExtraInhabitantCount(IGM));

    if (xiTagValues) {
      unsigned bitWidth = payloadTI.getFixedSize().getValueInBits();
      auto lowValue = payloadTI.getFixedExtraInhabitantValue(IGM, bitWidth, 0);
      auto mask = payloadTI.getFixedExtraInhabitantMask(IGM);

      auto tzCount = mask.countTrailingZeros();
      auto shiftedMask = mask.lshr(tzCount);
      auto toCount = shiftedMask.countTrailingOnes();
      if (mask.countPopulation() > 64 || toCount != mask.countPopulation() ||
          (tzCount % toCount != 0)) {
        // We currently don't handle cases with non-contiguous or > 64 bits of
        // extra inhabitants
        isSimple = false;
      } else {
        xiBitCount = std::min(64u, mask.countPopulation());
        xiBitOffset = mask.countTrailingZeros();
        zeroTagValue = lowValue.extractBitsAsZExtValue(xiBitCount, xiBitOffset);
      }
    }
  }

  if (xiTagValues < numEmptyCases) {
    if (payloadTI.getFixedSize() >= Size(4)) {
      extraTagByteCount = 1;
    } else {
      auto tagsWithoutInhabitants = numEmptyCases - xiTagValues;

      unsigned tagsPerTagBitValue =
          1 << payloadTI.getFixedSize().getValueInBits();
      auto numExtraTagValues =
          (tagsWithoutInhabitants + (tagsPerTagBitValue - 1)) /
              tagsPerTagBitValue +
          1;
      auto extraTagBitCount = llvm::Log2_32(numExtraTagValues - 1) + 1;
      extraTagByteCount = (extraTagBitCount + 7U) / 8U;
    }
  }

  LayoutStringBuilder payloadBuilder{};
  if (!cases[0]->refCountString(IGM, payloadBuilder, genericSig)) {
    return false;
  }

  if (isSimple) {
    B.addSinglePayloadEnumSimple(zeroTagValue, xiTagValues, extraTagByteCount,
                                 xiBitCount / 8, xiBitOffset / 8, cases[0]);
  } else {
    auto tagFn = createFixedEnumLoadTag(IGM, *this);
    B.addSinglePayloadEnumFN(tagFn, extraTagByteCount, cases[0]);
  }

  return true;
}

bool EnumTypeLayoutEntry::buildMultiPayloadRefCountString(
    IRGenModule &IGM, LayoutStringBuilder &B,
    GenericSignature genericSig) const {
  auto valid = std::all_of(cases.begin(), cases.end(), [&](auto *c) {
    LayoutStringBuilder nestedBuilder{};
    return c->refCountString(IGM, nestedBuilder, genericSig);
  });

  if (valid) {
    auto *tagFn = createFixedEnumLoadTag(IGM, *this);
    B.addMultiPayloadEnumFN(tagFn, this);
  }

  return valid;
}

llvm::Constant *
EnumTypeLayoutEntry::layoutString(IRGenModule &IGM,
                                  GenericSignature genericSig) const {
  if (_layoutString) {
    return *_layoutString;
  }

  auto createConstant = [&](const LayoutStringBuilder &B) {
    ConstantInitBuilder IB(IGM);
    auto SB = IB.beginStruct();
    SB.setPacked(true);

    B.result(IGM, SB, genericSig);

    IRGenMangler mangler;
    std::string symbolName =
        mangler.mangleSymbolNameForMangledMetadataAccessorString(
            "type_layout_string", genericSig.getCanonicalSignature(),
            ty.getASTType()->mapTypeOutOfContext()->getCanonicalType());

    auto *global = SB.finishAndCreateGlobal(symbolName, IGM.getPointerAlignment(),
                                             /*constant*/ true);

    IGM.setTrueConstGlobal(global);

    _layoutString = global;

    return global;
  };

  LayoutStringBuilder B{};

  switch (copyDestroyKind(IGM)) {
  case CopyDestroyStrategy::TriviallyDestroyable:
    return nullptr;

  case CopyDestroyStrategy::Normal: {
    bool valid = false;
    if (isFixedSize(IGM)) {
      if (isMultiPayloadEnum()) {
        valid = buildMultiPayloadRefCountString(IGM, B, genericSig);
      } else {
        valid = buildSinglePayloadRefCountString(IGM, B, genericSig);
      }
    }

    if (valid) {
      return createConstant(B);
    } else {
      return *(_layoutString = llvm::Optional<llvm::Constant *>(nullptr));
    }
  }

  case CopyDestroyStrategy::ForwardToPayload:
  case CopyDestroyStrategy::NullableRefcounted: {
    if (!isFixedSize(IGM) || isMultiPayloadEnum() ||
        !refCountString(IGM, B, genericSig)) {
      return *(_layoutString = llvm::Optional<llvm::Constant *>(nullptr));
    }

    return createConstant(B);
  }
  }
}

bool EnumTypeLayoutEntry::refCountString(IRGenModule &IGM,
                                         LayoutStringBuilder &B,
                                         GenericSignature genericSig) const {
  if (!isFixedSize(IGM)) return false;

  switch (copyDestroyKind(IGM)) {
  case CopyDestroyStrategy::TriviallyDestroyable: {
    auto size = fixedSize(IGM);
    assert(size && "POD should not have dynamic size");
    B.addSkip(size->getValue());
    return true;
  }
  case CopyDestroyStrategy::NullableRefcounted:
  case CopyDestroyStrategy::ForwardToPayload:
    return cases[0]->refCountString(IGM, B, genericSig);
  case CopyDestroyStrategy::Normal: {

    if (isMultiPayloadEnum() &&
        buildMultiPayloadRefCountString(IGM, B, genericSig)) {
      return true;
    } else if (buildSinglePayloadRefCountString(IGM, B, genericSig)) {
      return true;
    }

    // TODO: this is only relevant until we properly support enums.
    if (!ty.getASTType()->isLegalFormalType() ||
        !IGM.getSILModule().isTypeMetadataAccessible(ty.getASTType())) {
      return false;
    }

    auto *accessor = createMetatypeAccessorFunction(IGM, ty, genericSig);
    B.addFixedEnumRefCount(accessor);
    B.addSkip(fixedSize(IGM)->getValue());
    return true;
  }
  }
}

void EnumTypeLayoutEntry::computeProperties() {
  for (auto c: cases) {
    gatherProperties(c);
  }
}

EnumTypeLayoutEntry::CopyDestroyStrategy
EnumTypeLayoutEntry::copyDestroyKind(IRGenModule &IGM) const {
  if (isTriviallyDestroyable()) {
    return TriviallyDestroyable;
  } else if (isSingleton()) {
    return ForwardToPayload;
  } else if (cases.size() == 1 && numEmptyCases <= 1 &&
             cases[0]->isSingleRetainablePointer()) {
    return NullableRefcounted;
  } else {
    unsigned numTags = numEmptyCases;
    if (cases.size() == 1 &&
        cases[0]->canValueWitnessExtraInhabitantsUpTo(IGM, numTags - 1)) {
      return ForwardToPayload;
    }
    return Normal;
  }
}

llvm::Value *EnumTypeLayoutEntry::size(IRGenFunction &IGF) const {
  assert(!cases.empty());
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;
  auto &ctx = IGM.getLLVMContext();

  auto emptyCaseCount = IGM.getInt32(numEmptyCases);
  if (cases.size() == 1 && numEmptyCases == 0) {
    return cases[0]->size(IGF);
  } else if (cases.size() == 1) {
    // Single payload enum.
    // // If there are enough extra inhabitants for all of the cases, then the
    // // size of the enum is the same as its payload.
    //
    // size_t size;
    // if (payloadNumExtraInhabitants >= emptyCases) {
    //   size = payloadSize;
    // } else {
    //   size = payloadSize + getEnumTagCounts(payloadSize,
    //                                     emptyCases -
    //                                     payloadNumExtraInhabitants,
    //                                       1 /*payload case*/).numTagBytes;
    // }
    auto payloadXIs = cases[0]->extraInhabitantCount(IGF);
    auto payloadSize = cases[0]->size(IGF);
    auto truncPayloadSize =
        Builder.CreateZExtOrTrunc(cases[0]->size(IGF), IGM.Int32Ty);
    auto enoughXIs = Builder.CreateICmpUGE(payloadXIs, emptyCaseCount);
    auto branchBB = Builder.GetInsertBlock();
    auto resumeBB = llvm::BasicBlock::Create(ctx);
    auto computeTagBB = llvm::BasicBlock::Create(ctx);
    Builder.CreateCondBr(enoughXIs, resumeBB, computeTagBB);

    Builder.emitBlock(computeTagBB);
    auto extraEmptyCases = Builder.CreateSub(emptyCaseCount, payloadXIs);
    auto enumTagBytes =
        getEnumTagBytes(IGF, truncPayloadSize, extraEmptyCases, IGM.getInt32(1))
            .numTagBytes;
    auto payloadPlusEnumTagBytes = Builder.CreateAdd(
        payloadSize, Builder.CreateZExtOrTrunc(enumTagBytes, IGM.SizeTy));
    computeTagBB = Builder.GetInsertBlock();
    Builder.CreateBr(resumeBB);

    Builder.emitBlock(resumeBB);
    auto size = Builder.CreatePHI(IGM.SizeTy, 2);
    size->setName("size");
    size->addIncoming(payloadSize, branchBB);
    size->addIncoming(payloadPlusEnumTagBytes, computeTagBB);
    return size;
  }

  assert(cases.size() > 1);
  auto payloadSize = maxPayloadSize(IGF);
  auto truncPayloadSize =
      Builder.CreateZExtOrTrunc(payloadSize, IGM.Int32Ty);
  auto numPayloads = IGM.getInt32(cases.size());
  auto extraTagBytes =
    Builder.CreateZExtOrTrunc(
      getEnumTagBytes(IGF, truncPayloadSize, emptyCaseCount, numPayloads)
          .numTagBytes, IGM.SizeTy);
  return Builder.CreateAdd(payloadSize, extraTagBytes);
}

bool EnumTypeLayoutEntry::isFixedSize(IRGenModule &IGM) const {
  return fixedSize(IGM).has_value();
}

bool EnumTypeLayoutEntry::isTriviallyDestroyable() const {
  for (auto *entry : cases) {
    if (!entry->isTriviallyDestroyable()) {
      return false;
    }
  }
  return true;
}

bool EnumTypeLayoutEntry::canValueWitnessExtraInhabitantsUpTo(
    IRGenModule &IGM, unsigned index) const {
  if (cases.size() == 1) {
    if (numEmptyCases == 0) {
      return cases[0]->canValueWitnessExtraInhabitantsUpTo(IGM, index);
    }
    auto enumSize = fixedSize(IGM);
    if (!enumSize) {
      return false;
    }
    FixedEnumTagInfo tagInfo =
        getFixedEnumTagBytes(*enumSize, cases.size(), numEmptyCases);
    return cases[0]->canValueWitnessExtraInhabitantsUpTo(
        IGM, index + tagInfo.numTags);
  }
  return false;
}

bool EnumTypeLayoutEntry::isSingleRetainablePointer() const {
  return cases.size() == 1 && numEmptyCases == 0 &&
         cases[0]->isSingleRetainablePointer();
}

llvm::Optional<Size> EnumTypeLayoutEntry::fixedSize(IRGenModule &IGM) const {
  if (_fixedSize)
    return *_fixedSize;
  assert(!cases.empty());

  if (cases.size() == 1 && numEmptyCases == 0) {
    return cases[0]->fixedSize(IGM);
  } else if (cases.size() == 1) {
    // Single payload enum.
    //
    // If there are enough extra inhabitants for all of the cases, then the
    // size of the enum is the same as its payload.
    //
    Size size;
    auto payloadNumExtraInhabitants = cases[0]->fixedXICount(IGM);
    auto payloadSize = cases[0]->fixedSize(IGM);
    if (!payloadNumExtraInhabitants || !payloadSize) {
      return *(_fixedSize = llvm::Optional<Size>(llvm::None));
    }
    if (*payloadNumExtraInhabitants >= numEmptyCases) {
      size = *payloadSize;
    } else {
      size = Size((*payloadSize).getValue() +
                  getFixedEnumTagBytes(
                      *payloadSize, numEmptyCases - *payloadNumExtraInhabitants,
                      1 /*payload case*/)
                      .numTagBytes);
    }
    return *(_fixedSize = size);
  }

  assert(cases.size() > 1);
  Size maxPayloadSize(0);
  for (auto enum_case : cases) {
    auto caseSize = enum_case->fixedSize(IGM);
    if (!caseSize) {
      return *(_fixedSize = llvm::Optional<Size>(llvm::None));
    }
    maxPayloadSize = std::max(*caseSize, maxPayloadSize);
  }
  uint32_t truncPayloadSize = maxPayloadSize.getValue();
  auto extraTagBytes =
      getFixedEnumTagBytes(Size(truncPayloadSize), numEmptyCases, cases.size())
          .numTagBytes;
  return *(_fixedSize = maxPayloadSize + Size(extraTagBytes));
}

llvm::Optional<Alignment>
EnumTypeLayoutEntry::fixedAlignment(IRGenModule &IGM) const {
  if (_fixedAlignment)
    return *_fixedAlignment;
  Alignment maxAlign(minimumAlignment);
  for (auto payload : cases) {
    auto caseAlign = payload->fixedAlignment(IGM);
    if (!caseAlign) {
      return *(_fixedAlignment = llvm::Optional<Alignment>(llvm::None));
    }
    maxAlign = std::max(*caseAlign, maxAlign);
  }
  return *(_fixedAlignment = maxAlign);
}

llvm::Optional<uint32_t>
EnumTypeLayoutEntry::fixedXICount(IRGenModule &IGM) const {
  if (_fixedXICount)
    return *_fixedXICount;
  assert(!cases.empty());

  if (cases.size() == 1 && numEmptyCases == 0) {
    return cases[0]->fixedXICount(IGM);
  } else if (cases.size() == 1) {
    // Single payload enum.
    // unsigned unusedExtraInhabitants =
    //   payloadNumExtraInhabitants >= emptyCases ?
    //     payloadNumExtraInhabitants - emptyCases : 0;
    auto payloadXIs = cases[0]->fixedXICount(IGM);
    if (!payloadXIs) {
      return *(_fixedXICount = llvm::Optional<uint32_t>(llvm::None));
    }
    return *(_fixedXICount =
                 payloadXIs >= numEmptyCases ? *payloadXIs - numEmptyCases : 0);
  }

  // See whether there are extra inhabitants in the tag.
  // unsigned numExtraInhabitants = tagCounts.numTagBytes == 4
  //   ? INT_MAX
  //   : (1 << (tagCounts.numTagBytes * 8)) - tagCounts.numTags;
  // numExtraInhabitants = std::min(numExtraInhabitants,
  //                       unsigned(ValueWitnessFlags::MaxNumExtraInhabitants));
  Size maxPayloadSize(0);
  for (auto enum_case : cases) {
    auto caseSize = enum_case->fixedSize(IGM);
    if (!caseSize) {
      return *(_fixedXICount = llvm::Optional<uint32_t>(llvm::None));
    }
    maxPayloadSize = std::max(*caseSize, maxPayloadSize);
  }
  auto extraTagInfo =
      getFixedEnumTagBytes(maxPayloadSize, numEmptyCases, cases.size());
  uint32_t extraTagBytes = extraTagInfo.numTagBytes;
  uint32_t numTags = extraTagInfo.numTags;
  if (extraTagBytes == 4) {
    return *(_fixedXICount = 0x7FFFFFFF);
  } else {
    uint32_t numXIs = (1 << extraTagBytes * 8) - numTags;
    return *(_fixedXICount = std::min(
                 (uint32_t)0x7FFFFFFF, numXIs));
  }
}

llvm::Value *EnumTypeLayoutEntry::extraInhabitantCount(IRGenFunction &IGF) const {
  assert(!cases.empty());
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;

  if (cases.size() == 1 && numEmptyCases == 0) {
    return cases[0]->extraInhabitantCount(IGF);
  } else if (cases.size() == 1) {
    // Single payload enum.
    // unsigned unusedExtraInhabitants =
    //   payloadNumExtraInhabitants >= emptyCases ?
    //     payloadNumExtraInhabitants - emptyCases : 0;
    auto emptyCaseCount = llvm::ConstantInt::get(IGM.Int32Ty, numEmptyCases);
    auto payloadXIs = cases[0]->extraInhabitantCount(IGF);
    auto enoughXIs = Builder.CreateICmpUGE(payloadXIs, emptyCaseCount);
    auto remainingXIs = Builder.CreateSub(payloadXIs, emptyCaseCount);
    auto zero = IGM.getInt32(0);
    auto unusedXIs = Builder.CreateSelect(enoughXIs, remainingXIs, zero);
    unusedXIs->setName("num-extra-inhabitants");
    return unusedXIs;
  }

  assert(cases.size() > 1);
  auto one = IGM.getInt32(1);
  auto four = IGM.getInt32(4);
  auto eight = IGM.getInt32(8);
  auto intMax = IGM.getInt32(INT_MAX);

  // See whether there are extra inhabitants in the tag.
  // unsigned numExtraInhabitants = tagCounts.numTagBytes == 4
  //   ? INT_MAX
  //   : (1 << (tagCounts.numTagBytes * 8)) - tagCounts.numTags;
  // numExtraInhabitants = std::min(numExtraInhabitants,
  //                       unsigned(ValueWitnessFlags::MaxNumExtraInhabitants));
  auto payloadSize =
      Builder.CreateZExtOrTrunc(maxPayloadSize(IGF), IGM.Int32Ty);
  auto numPayloads = IGM.getInt32(cases.size());
  auto emptyCaseCount = IGM.getInt32(numEmptyCases);
  auto extraTagInfo =
      getEnumTagBytes(IGF, payloadSize, emptyCaseCount, numPayloads);
  auto maxNumXIs = llvm::ConstantInt::get(
      IGM.Int32Ty, ValueWitnessFlags::MaxNumExtraInhabitants);
  auto extraTagBytes =
      Builder.CreateZExtOrTrunc(extraTagInfo.numTagBytes, IGM.Int32Ty);
  auto numXIs = Builder.CreateMul(extraTagBytes, eight);
  numXIs = Builder.CreateShl(one, numXIs);
  auto numTags = Builder.CreateZExtOrTrunc(extraTagInfo.numTags, IGM.Int32Ty);
  numXIs = Builder.CreateSub(numXIs, numTags);
  auto fourTagBytes = Builder.CreateICmpEQ(extraTagBytes, four);
  numXIs = Builder.CreateSelect(fourTagBytes, intMax, numXIs);
  auto lte = Builder.CreateICmpULE(numXIs, maxNumXIs);
  numXIs = Builder.CreateSelect(lte, numXIs, maxNumXIs);
  numXIs->setName("num-extra-inhabitants");
  return numXIs;
}

static void emitMemCpy(IRGenFunction &IGF, Address dest, Address src,
                       llvm::Value *size) {
  auto &Builder = IGF.Builder;
  auto &IGM = IGF.IGM;

  // If the layout is fixed, the size will be a constant.
  // Otherwise, do a memcpy of the dynamic size of the type.
  auto byteDestAddr = Builder.CreateBitOrPointerCast(dest.getAddress(), IGM.Int8PtrTy);
  auto byteSrcAddr =
      Builder.CreateBitOrPointerCast(src.getAddress(), IGM.Int8PtrTy);
  Builder.CreateMemCpy(byteDestAddr, llvm::MaybeAlign(dest.getAlignment()),
                       byteSrcAddr, llvm::MaybeAlign(src.getAlignment()), size);
}

llvm::BasicBlock *
EnumTypeLayoutEntry::testSinglePayloadEnumContainsPayload(IRGenFunction &IGF,
                                                          Address addr) const {
  assert(cases.size() == 1);
  auto &Builder = IGF.Builder;
  auto &IGM = IGF.IGM;

  auto emptyCases = IGM.getInt32(numEmptyCases);
  auto tag = cases[0]->getEnumTagSinglePayload(IGF, emptyCases, addr);
  auto payloadBB = IGF.createBasicBlock("payloadBlock");
  auto noPayloadBB = IGF.createBasicBlock("noPayloadBlock");
  auto hasPayload = Builder.CreateICmpEQ(tag, IGM.getInt32(0));
  Builder.CreateCondBr(hasPayload, payloadBB, noPayloadBB);

  Builder.emitBlock(payloadBB);
  return noPayloadBB;
}

void EnumTypeLayoutEntry::initializeSinglePayloadEnum(IRGenFunction &IGF,
                                                      Address dest, Address src,
                                                      IsTake_t isTake) const {
  assert(cases.size() == 1);

  Address destData = dest;
  Address srcData = src;
  auto payload = cases[0];
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;

  switch (copyDestroyKind(IGM)) {
  case TriviallyDestroyable: {
    emitMemCpy(IGF, dest, src, size(IGF));
    break;
  }
  case NullableRefcounted:
  case ForwardToPayload: {
    // Here, the source value has a payload. Initialize the destination
    // with it, and set the extra tag if any to zero.
    payload->initialize(IGF, destData, srcData, isTake);
    // Potentially initialize extra tag bytes.
    payload->storeEnumTagSinglePayload(IGF, IGM.getInt32(0),
                                       IGM.getInt32(numEmptyCases), dest);
    break;
  }
  case Normal: {
    auto endBB = IGF.createBasicBlock("");

    // See whether the source value has a payload.
    auto noSrcPayloadBB = testSinglePayloadEnumContainsPayload(IGF, src);

    {
      ConditionalDominanceScope condition(IGF);

      // Here, the source value has a payload. Initialize the destination
      // with it, and set the extra tag if any to zero.
      payload->initialize(IGF, destData, srcData, isTake);
      // Potentially initialize extra tag bytes.
      payload->storeEnumTagSinglePayload(IGF, IGM.getInt32(0),
                                         IGM.getInt32(numEmptyCases), dest);
      Builder.CreateBr(endBB);
    }

    // If the source value has no payload, we can primitive-store the
    // empty-case value.
    Builder.emitBlock(noSrcPayloadBB);
    {
      ConditionalDominanceScope condition(IGF);
      emitMemCpy(IGF, dest, src, size(IGF));
      Builder.CreateBr(endBB);
    }

    IGF.Builder.emitBlock(endBB);
    break;
  }
  }
}

void EnumTypeLayoutEntry::assignSinglePayloadEnum(IRGenFunction &IGF,
                                                  Address dest, Address src,
                                                  IsTake_t isTake) const {
  assert(cases.size() == 1);
  Address destData = dest;
  Address srcData = src;

  auto payload = cases[0];
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;

  switch (copyDestroyKind(IGM)) {
  case TriviallyDestroyable: {
    emitMemCpy(IGF, dest, src, size(IGF));
    break;
  }
  case ForwardToPayload:
  case NullableRefcounted: {
    payload->assign(IGF, destData, srcData, isTake);
    break;
  }
  case Normal: {
    auto endBB = IGF.createBasicBlock("");

    // See whether the current value at the destination has a payload.
    auto *noDestPayloadBB = testSinglePayloadEnumContainsPayload(IGF, dest);
    {
      ConditionalDominanceScope destCondition(IGF);

      // Here, the destination has a payload. Now see if the source also
      // has one.
      auto destNoSrcPayloadBB = testSinglePayloadEnumContainsPayload(IGF, src);
      {
        ConditionalDominanceScope destSrcCondition(IGF);

        // Here, both source and destination have payloads. Do the
        // reassignment of the payload in-place.
        payload->assign(IGF, destData, srcData, isTake);
        Builder.CreateBr(endBB);
      }

      // If the destination has a payload but the source doesn't, we can
      // destroy the payload and primitive-store the new no-payload value.
      Builder.emitBlock(destNoSrcPayloadBB);
      {
        ConditionalDominanceScope destNoSrcCondition(IGF);
        payload->destroy(IGF, destData);
        emitMemCpy(IGF, dest, src, this->size(IGF));
        Builder.CreateBr(endBB);
      }
    }

    // Now, if the destination has no payload, check if the source has one.
    Builder.emitBlock(noDestPayloadBB);
    {
      ConditionalDominanceScope noDestCondition(IGF);
      auto noDestNoSrcPayloadBB =
          testSinglePayloadEnumContainsPayload(IGF, src);
      {
        ConditionalDominanceScope noDestSrcCondition(IGF);

        // Here, the source has a payload but the destination doesn't.
        // We can copy-initialize the source over the destination, then
        // primitive-store the zero extra tag (if any).
        payload->initialize(IGF, destData, srcData, isTake);
        // Potentially initialize extra tag bytes.
        payload->storeEnumTagSinglePayload(IGF, IGM.getInt32(0),
                                           IGM.getInt32(numEmptyCases), dest);
        Builder.CreateBr(endBB);
      }

      // If neither destination nor source have payloads, we can just
      // primitive-store the new empty-case value.
      Builder.emitBlock(noDestNoSrcPayloadBB);
      {
        ConditionalDominanceScope noDestNoSrcCondition(IGF);
        emitMemCpy(IGF, dest, src, this->size(IGF));
        Builder.CreateBr(endBB);
      }
    }

    Builder.emitBlock(endBB);
    break;
  }
  }
}

void EnumTypeLayoutEntry::multiPayloadEnumForPayloadAndEmptyCases(
    IRGenFunction &IGF, Address addr,
    llvm::function_ref<void(TypeLayoutEntry *payload, llvm::Value *tagIndex)>
        payloadFunction,
    llvm::function_ref<void()> noPayloadFunction) const {
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;
  auto &ctxt = IGM.getLLVMContext();

  auto tag = getEnumTagMultipayload(IGF, addr);
  auto *endBB = llvm::BasicBlock::Create(ctxt);

  auto *trivialBB = llvm::BasicBlock::Create(ctxt);
  bool anyTrivial = numEmptyCases != 0;
  unsigned numPayloads = cases.size();

  auto switchBuilder = SwitchBuilder::create(
      IGF, tag,
      SwitchDefaultDest(trivialBB,
                        anyTrivial ? IsNotUnreachable : IsUnreachable),
      numPayloads);

  unsigned tagIndex = 0;
  // Payload cases start at 0.
  for (auto &payload : cases) {
    auto *caseBB = llvm::BasicBlock::Create(ctxt);
    auto *tag = IGM.getInt32(tagIndex);
    switchBuilder->addCase(tag, caseBB);
    Builder.emitBlock(caseBB);
    {
      ConditionalDominanceScope scope(IGF);
      payloadFunction(payload, tag);
    }
    Builder.CreateBr(endBB);
    ++tagIndex;
  }

  if (anyTrivial) {
    Builder.emitBlock(trivialBB);
    {
      ConditionalDominanceScope scope(IGF);
      noPayloadFunction();
    }
    Builder.CreateBr(endBB);
  } else {
    // If there are no trivial cases to handle, this is unreachable.
    if (trivialBB->use_empty()) {
      delete trivialBB;
    } else {
      Builder.emitBlock(trivialBB);
      Builder.CreateUnreachable();
    }
  }

  Builder.emitBlock(endBB);
}

void EnumTypeLayoutEntry::destroyMultiPayloadEnum(IRGenFunction &IGF,
                                                  Address addr) const {
  multiPayloadEnumForPayloadAndEmptyCases(
      IGF, addr,
      [&](TypeLayoutEntry *payload, llvm::Value *) {
        payload->destroy(IGF, addr);
      },
      []() { /* nothing to do */ });
}

void EnumTypeLayoutEntry::assignMultiPayloadEnum(IRGenFunction &IGF,
                                                 Address dest, Address src,
                                                 IsTake_t isTake) const {
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;
  auto &ctxt = IGM.getLLVMContext();
  auto *endBB = llvm::BasicBlock::Create(ctxt);

  // Check whether the source and destination alias.
  llvm::Value *alias =
      Builder.CreateICmpEQ(dest.getAddress(), src.getAddress());
  auto *noAliasBB = llvm::BasicBlock::Create(ctxt);
  Builder.CreateCondBr(alias, endBB, noAliasBB);
  Builder.emitBlock(noAliasBB);
  {
    ConditionalDominanceScope condition(IGF);

    // Destroy the old value.
    destroyMultiPayloadEnum(IGF, dest);

    // Reinitialize with the new value.
    initializeMultiPayloadEnum(IGF, dest, src, isTake);

    IGF.Builder.CreateBr(endBB);
  }
  IGF.Builder.emitBlock(endBB);
}

void EnumTypeLayoutEntry::initializeMultiPayloadEnum(IRGenFunction &IGF,
                                                     Address dest, Address src,
                                                     IsTake_t isTake) const {
  multiPayloadEnumForPayloadAndEmptyCases(
      IGF, src,
      [&](TypeLayoutEntry *payload, llvm::Value *tagIndex) {
        if (isTake)
          payload->initWithTake(IGF, dest, src);
        else
          payload->initWithCopy(IGF, dest, src);
        storeMultiPayloadTag(IGF, tagIndex, dest);
      },
      [&]() { emitMemCpy(IGF, dest, src, this->size(IGF)); });
}

void EnumTypeLayoutEntry::destroySinglePayloadEnum(IRGenFunction &IGF,
                                                   Address addr) const {
  switch (copyDestroyKind(IGF.IGM)) {
  case TriviallyDestroyable: {
    break;
  }
  case ForwardToPayload:
  case NullableRefcounted: {
    auto payload = cases[0];
    payload->destroy(IGF, addr);
    break;
  }
  case Normal: {
    // Check that there is a payload at the address.
    llvm::BasicBlock *endBB = testSinglePayloadEnumContainsPayload(IGF, addr);
    {
      ConditionalDominanceScope condition(IGF);

      // If there is, destroy it.
      auto payload = cases[0];
      payload->destroy(IGF, addr);

      IGF.Builder.CreateBr(endBB);
    }
    IGF.Builder.emitBlock(endBB);
    break;
  }
  }
}

void EnumTypeLayoutEntry::destroy(IRGenFunction &IGF, Address addr) const {
  assert(!cases.empty());

  if (cases.size() == 1) {
    destroySinglePayloadEnum(IGF, addr);
    return;
  }

  destroyMultiPayloadEnum(IGF, addr);
}

void EnumTypeLayoutEntry::assignWithCopy(IRGenFunction &IGF, Address dest,
                                         Address src) const {
  assert(!cases.empty());
  if (cases.size() == 1) {
    return assignSinglePayloadEnum(IGF, dest, src, IsNotTake);
  }
  assert(cases.size() > 1);
  assignMultiPayloadEnum(IGF, dest, src, IsNotTake);
}

void EnumTypeLayoutEntry::assignWithTake(IRGenFunction &IGF, Address dest,
                                         Address src) const {
  assert(!cases.empty());
  if (cases.size() == 1) {
    return assignSinglePayloadEnum(IGF, dest, src, IsTake);
  }

  assert(cases.size() > 1);
  assignMultiPayloadEnum(IGF, dest, src, IsTake);
}

void EnumTypeLayoutEntry::initWithCopy(IRGenFunction &IGF, Address dest,
                                       Address src) const {
  assert(!cases.empty());
  if (cases.size() == 1) {
    return initializeSinglePayloadEnum(IGF, dest, src, IsNotTake);
  }

  assert(cases.size() > 1);
  initializeMultiPayloadEnum(IGF, dest, src, IsNotTake);
}

void EnumTypeLayoutEntry::initWithTake(IRGenFunction &IGF, Address dest,
                                       Address src) const {
  assert(!cases.empty());
  if (cases.size() == 1) {
    return initializeSinglePayloadEnum(IGF, dest, src, IsTake);
  }

  assert(cases.size() > 1);
  initializeMultiPayloadEnum(IGF, dest, src, IsTake);
}

std::pair<Address, llvm::Value *>
EnumTypeLayoutEntry::getMultiPayloadEnumTagByteAddrAndNumBytes(
    IRGenFunction &IGF, Address addr) const {
  auto &Builder = IGF.Builder;
  auto &IGM = IGF.IGM;
  auto payloadSize = maxPayloadSize(IGF);
  auto *valueAddr =
      Builder.CreateBitOrPointerCast(addr.getAddress(), IGM.Int8PtrTy);
  auto extraTagBytesAddr =
      Address(Builder.CreateInBoundsGEP(IGM.Int8Ty, valueAddr, payloadSize),
              IGM.Int8Ty, Alignment(1));
  auto numPayloads = IGM.getInt32(cases.size());
  auto emptyCaseCount = IGM.getInt32(numEmptyCases);

  auto truncPayloadSize = Builder.CreateZExtOrTrunc(payloadSize, IGM.Int32Ty);
  auto extraTagInfo =
      getEnumTagBytes(IGF, truncPayloadSize, emptyCaseCount, numPayloads);

  return std::make_pair(extraTagBytesAddr, extraTagInfo.numTagBytes);
}

llvm::Value *EnumTypeLayoutEntry::getEnumTagSinglePayloadForMultiPayloadEnum(
    IRGenFunction &IGF, Address addr, llvm::Value *emptyCases) const {
  // We don't handle (create enum type layout entries) multi payload enums that
  // have known spare bits (enums that are always fixed size) . Therefore the
  // only place to store extra inhabitants is in available bits in the extra tag
  // bytes.
  auto &Builder = IGF.Builder;
  auto &IGM = IGF.IGM;
  return getEnumTagSinglePayloadGeneric(
      IGF, addr, emptyCases, [&](Address addr) -> llvm::Value * {
        // Compute the address and number of the tag bytes.
        Address extraTagBitsAddr;
        llvm::Value *numTagBytes;
        std::tie(extraTagBitsAddr, numTagBytes) =
            getMultiPayloadEnumTagByteAddrAndNumBytes(IGF, addr);

        // Compute the tag.
        // unsigned tag;
        // if (numTagBytes == 4)
        //   tag = loadedTag;
        // else
        //   tag = loadedTag | (~0u & (~0u << (numTagBytes * 8)));
        auto loadedTag = emitLoad1to4Bytes(IGF, extraTagBitsAddr, numTagBytes);
        auto tag = llvm::PHINode::Create(IGM.Int32Ty, 2);
        auto four = IGM.getInt32(4);
        auto hasFourTagBytes = Builder.CreateICmpEQ(numTagBytes, four);
        tag->addIncoming(loadedTag, Builder.GetInsertBlock());
        auto tagComputedBB = IGF.createBasicBlock("");
        auto lessThanFourBytesBB = IGF.createBasicBlock("");
        Builder.CreateCondBr(hasFourTagBytes, tagComputedBB, lessThanFourBytesBB);

        Builder.emitBlock(lessThanFourBytesBB);
        auto baseValue = IGM.getInt32(~0u);
        auto shifted = Builder.CreateShl(
            baseValue, Builder.CreateMul(numTagBytes, IGM.getInt32(8)));
        auto newTag =
            Builder.CreateOr(loadedTag, Builder.CreateAnd(baseValue, shifted));
        tag->addIncoming(newTag, Builder.GetInsertBlock());
        Builder.CreateBr(tagComputedBB);

        Builder.emitBlock(tagComputedBB);
        Builder.Insert(tag);

        // index = ~tag;
        // if (index >= extraInhabitantCount)
        //   index = 0;
        // else
        //   index = index + 1;
        auto index = Builder.CreateNot(tag);

        auto result = llvm::PHINode::Create(IGM.Int32Ty, 2);
        auto resultBB = IGF.createBasicBlock("");
        auto addOneBB = IGF.createBasicBlock("");
        auto indexGEQXICount =
            Builder.CreateICmpUGE(index, extraInhabitantCount(IGF));
        result->addIncoming(IGM.getInt32(0), Builder.GetInsertBlock());
        Builder.CreateCondBr(indexGEQXICount, resultBB, addOneBB);

        Builder.emitBlock(addOneBB);
        result->addIncoming(Builder.CreateAdd(index, IGM.getInt32(1)),
                            Builder.GetInsertBlock());
        Builder.CreateBr(resultBB);

        Builder.emitBlock(resultBB);
        Builder.Insert(result);
        return result;
      });
}

llvm::Value *EnumTypeLayoutEntry::getEnumTagSinglePayloadForSinglePayloadEnum(
    IRGenFunction &IGF, Address addr, llvm::Value *emptyCases) const {
  assert(cases.size() == 1);
  return getEnumTagSinglePayloadGeneric(
      IGF, addr, emptyCases, [&](Address addr) -> llvm::Value * {
        auto payloadEntry = cases[0];
        auto maxNumXIPayload = payloadEntry->extraInhabitantCount(IGF);
        // Read the tag from the payload and adjust it by the number
        // cases of this enum.
        auto tag =
            payloadEntry->getEnumTagSinglePayload(IGF, maxNumXIPayload, addr);
        auto numEnumCases = IGF.IGM.getInt32(numEmptyCases);
        auto adjustedTag = IGF.Builder.CreateSub(tag, numEnumCases);
        auto isEnumValue = IGF.Builder.CreateICmpULE(tag, numEnumCases);
        adjustedTag = IGF.Builder.CreateSelect(isEnumValue, IGF.IGM.getInt32(0),
                                               adjustedTag);
        return adjustedTag;
      });
}

llvm::Value *EnumTypeLayoutEntry::getEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *emptyCases, Address addr) const {
  assert(!cases.empty());
  if (cases.size() == 1 && numEmptyCases == 0) {
    return cases[0]->getEnumTagSinglePayload(IGF, emptyCases, addr);
  } else if (cases.size() == 1) {
    return getEnumTagSinglePayloadForSinglePayloadEnum(IGF, addr, emptyCases);
  }
  return getEnumTagSinglePayloadForMultiPayloadEnum(IGF, addr, emptyCases);
}

void EnumTypeLayoutEntry::storeEnumTagSinglePayloadForSinglePayloadEnum(
    IRGenFunction &IGF, llvm::Value *tag, llvm::Value *emptyCases,
    Address addr) const {
  assert(cases.size() == 1);
  storeEnumTagSinglePayloadGeneric(
      IGF, tag, emptyCases, addr, [&](Address addr, llvm::Value *tag) {
        auto payloadEntry = cases[0];
        auto maxNumXIPayload = payloadEntry->extraInhabitantCount(IGF);
        auto numExtraCases = IGF.IGM.getInt32(numEmptyCases);
        // Adjust the tag.
        llvm::Value *adjustedTag = IGF.Builder.CreateAdd(tag, numExtraCases);

        // Preserve the zero tag so that we don't pass down a meaningless XI
        // value that the payload will waste time installing before we
        // immediately overwrite it.
        auto isEnumValue = IGF.Builder.CreateIsNull(tag);
        adjustedTag = IGF.Builder.CreateSelect(isEnumValue, IGF.IGM.getInt32(0),
                                               adjustedTag);
        payloadEntry->storeEnumTagSinglePayload(IGF, adjustedTag,
                                                maxNumXIPayload, addr);

      });
}

void EnumTypeLayoutEntry::storeMultiPayloadTag(IRGenFunction &IGF,
                                               llvm::Value *value,
                                               Address enumAddr) const {
  // Compute the address and number of the tag bytes.
  Address extraTagBytesAddr;
  llvm::Value *numTagBytes;
  std::tie(extraTagBytesAddr, numTagBytes) =
      getMultiPayloadEnumTagByteAddrAndNumBytes(IGF, enumAddr);

  emitStore1to4Bytes(IGF, extraTagBytesAddr, value, numTagBytes);
}

void EnumTypeLayoutEntry::storeMultiPayloadValue(IRGenFunction &IGF,
                                                 llvm::Value *value,
                                                 Address enumAddr) const {
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;
  auto truncSize = Builder.CreateZExtOrTrunc(maxPayloadSize(IGF), IGM.Int32Ty);
  auto four = IGM.getInt32(4);
  auto sizeGTE4 = Builder.CreateICmpUGE(truncSize, four);
  auto sizeClampedTo4 = Builder.CreateSelect(sizeGTE4, four, truncSize);
  Builder.CreateMemSet(enumAddr, llvm::ConstantInt::get(IGF.IGM.Int8Ty, 0),
                       truncSize);
  emitStore1to4Bytes(IGF, enumAddr, value, sizeClampedTo4);
}

void EnumTypeLayoutEntry::storeEnumTagSinglePayloadForMultiPayloadEnum(
    IRGenFunction &IGF, llvm::Value *tag, llvm::Value *emptyCases,
    Address addr) const {
  // We don't handle (create enum type layout entries) multi payload enums that
  // have known spare bits (enums that are always fixed size). Therefore the
  // only place to store extra inhabitants is in available bits in the extra tag
  // bytes.
  auto &Builder = IGF.Builder;
  auto &IGM = IGF.IGM;
  storeEnumTagSinglePayloadGeneric(
      IGF, tag, emptyCases, addr, [&](Address addr, llvm::Value *tag) {
        ConditionalDominanceScope scope(IGF);
        auto invertedTag =
            Builder.CreateNot(Builder.CreateSub(tag, IGM.getInt32(1)));
        storeMultiPayloadTag(IGF, invertedTag, addr);
      });
}

void EnumTypeLayoutEntry::storeEnumTagSinglePayload(IRGenFunction &IGF,
                                                    llvm::Value *tag,
                                                    llvm::Value *emptyCases,
                                                    Address addr) const {
  assert(!cases.empty());
  if (cases.size() == 1 && numEmptyCases == 0) {
    return cases[0]->storeEnumTagSinglePayload(IGF, tag, emptyCases, addr);
  } else if (cases.size() == 1) {
    storeEnumTagSinglePayloadForSinglePayloadEnum(IGF, tag, emptyCases, addr);
    return;
  }

  storeEnumTagSinglePayloadForMultiPayloadEnum(IGF, tag, emptyCases, addr);
}

bool EnumTypeLayoutEntry::isMultiPayloadEnum() const {
  return cases.size() > 1;
}

bool EnumTypeLayoutEntry::isSingleton() const {
  return cases.size() + numEmptyCases == 1;
}

llvm::Value *
EnumTypeLayoutEntry::getEnumTagMultipayload(IRGenFunction &IGF,
                                            Address enumAddr) const {
  auto &IGM = IGF.IGM;
  // unsigned tag = loadMultiPayloadTag(value, layout);
  // if (tag < numPayloads) {
  //   // If the tag indicates a payload, then we're done.
  //   return tag;
  // } else {
  //   // Otherwise, the other part of the discriminator is in the payload.
  //   unsigned payloadValue = loadMultiPayloadValue(value, layout);
  //   if (layout.payloadSize >= 4) {
  //     return numPayloads + payloadValue;
  //   } else {
  //     unsigned numPayloadBits = layout.payloadSize * CHAR_BIT;
  //     return (payloadValue | (tag - numPayloads) << numPayloadBits)
  //            + numPayloads;
  //   }
  // }
  auto &Builder = IGF.Builder;
  Address extraTagBitsAddr;
  llvm::Value *numTagBytes;
  std::tie(extraTagBitsAddr, numTagBytes) =
      getMultiPayloadEnumTagByteAddrAndNumBytes(IGF, enumAddr);
  auto loadedTag = emitLoad1to4Bytes(IGF, extraTagBitsAddr, numTagBytes);
  auto resultBB = IGF.createBasicBlock("result");
  auto usePayloadBB = IGF.createBasicBlock("use-payload-for-tag");
  auto numPayloads = IGM.getInt32(cases.size());
  auto usePayloadValue = Builder.CreateICmpUGE(loadedTag, numPayloads);
  auto tagValue = llvm::PHINode::Create(IGM.Int32Ty, 4);
  tagValue->addIncoming(loadedTag, Builder.GetInsertBlock());
  Builder.CreateCondBr(usePayloadValue, usePayloadBB, resultBB);

  Builder.emitBlock(usePayloadBB);
  auto four = IGM.getInt32(4);
  auto truncSize = Builder.CreateZExtOrTrunc(maxPayloadSize(IGF), IGM.Int32Ty);
  auto sizeGTE4 = Builder.CreateICmpUGE(truncSize, four);
  auto sizeClampedTo4 = Builder.CreateSelect(sizeGTE4, four, truncSize);
  auto sizeGreaterZeroBB = IGF.createBasicBlock("");
  auto zero = IGM.getInt32(0);
  auto sizeGreaterZero = Builder.CreateICmpUGT(sizeClampedTo4, zero);
  tagValue->addIncoming(loadedTag, Builder.GetInsertBlock());
  Builder.CreateCondBr(sizeGreaterZero, sizeGreaterZeroBB, resultBB);

  Builder.emitBlock(sizeGreaterZeroBB);
  auto payloadValue = emitLoad1to4Bytes(IGF, enumAddr, sizeClampedTo4);
  auto payloadGTE4BB = IGF.createBasicBlock("");
  auto payloadLT4BB = IGF.createBasicBlock("");
  auto isPayloadGTE4 = Builder.CreateICmpUGE(truncSize, four);
  Builder.CreateCondBr(isPayloadGTE4, payloadGTE4BB, payloadLT4BB);

  Builder.emitBlock(payloadGTE4BB);
  auto result2 = Builder.CreateAdd(numPayloads, payloadValue);
  tagValue->addIncoming(result2, Builder.GetInsertBlock());
  Builder.CreateBr(resultBB);

  Builder.emitBlock(payloadLT4BB);
  auto numPayloadBits = Builder.CreateMul(truncSize, IGM.getInt32(CHAR_BIT));
  auto tmp = Builder.CreateSub(loadedTag, numPayloads);
  auto tmp2 = Builder.CreateShl(tmp, numPayloadBits);
  auto tmp3 = Builder.CreateOr(payloadValue, tmp2);
  auto result3 = Builder.CreateAdd(tmp3, numPayloads);
  tagValue->addIncoming(result3, Builder.GetInsertBlock());
  Builder.CreateBr(resultBB);

  Builder.emitBlock(resultBB);
  Builder.Insert(tagValue);
  return tagValue;
}

llvm::Value *EnumTypeLayoutEntry::getEnumTag(IRGenFunction &IGF,
                                             Address enumAddr) const {
  assert(!cases.empty());

  if (isSingleton()) {
    // Singleton tag is always `0`
    return IGF.IGM.getInt32(0);
  } else if (cases.size() == 1) {
    // Single payload enum.
    auto &IGM = IGF.IGM;
    auto payload = cases[0];
    auto emptyCases = IGM.getInt32(numEmptyCases);
    return payload->getEnumTagSinglePayload(IGF, emptyCases, enumAddr);
  }

  return getEnumTagMultipayload(IGF, enumAddr);
}

void EnumTypeLayoutEntry::destructiveProjectEnumData(IRGenFunction &IGF,
                                                     Address enumAddr) const {

  if (cases.size() == 1) {
    // Nothing to do because single payload enums don't interleave tag bits.
    return;
  }
  // Nothing to do here either because we don't handle fixed size enums which
  // would be the only ones to use spare bits in the payload.
}

void EnumTypeLayoutEntry::storeEnumTagMultipayload(IRGenFunction &IGF,
                                                   llvm::Value *tag,
                                                   Address enumAddr) const {
  // if (whichCase < numPayloads) {
  //   // For a payload case, store the tag after the payload area.
  //   storeMultiPayloadTag(value, layout, whichCase);
  // } else {
  //   // For an empty case, factor out the parts that go in the payload and
  //   // tag areas.
  //   unsigned whichEmptyCase = whichCase - numPayloads;
  //   unsigned whichTag, whichPayloadValue;
  //   if (layout.payloadSize >= 4) {
  //     whichTag = numPayloads;
  //     whichPayloadValue = whichEmptyCase;
  //   } else {
  //     unsigned numPayloadBits = layout.payloadSize * CHAR_BIT;
  //     whichTag = numPayloads + (whichEmptyCase >> numPayloadBits);
  //     whichPayloadValue = whichEmptyCase & ((1U << numPayloadBits) - 1U);
  //   }
  //   storeMultiPayloadTag(value, layout, whichTag);
  //   storeMultiPayloadValue(value, layout, whichPayloadValue);
  // }
  auto &IGM = IGF.IGM;
  auto &Builder = IGF.Builder;

  auto numPayloads = IGM.getInt32(cases.size());
  auto shouldStoreOnlyTag = Builder.CreateICmpULT(tag, numPayloads);
  auto tagOnlyBB = IGF.createBasicBlock("tag-only");
  auto bothBB = IGF.createBasicBlock("tag-and-payload");
  auto finishedBB = IGF.createBasicBlock("");
  Builder.CreateCondBr(shouldStoreOnlyTag, tagOnlyBB, bothBB);

  Builder.emitBlock(tagOnlyBB);
  {
    ConditionalDominanceScope scope(IGF);
    storeMultiPayloadTag(IGF, tag, enumAddr);
  }
  Builder.CreateBr(finishedBB);

  Builder.emitBlock(bothBB);
  {
    ConditionalDominanceScope scope(IGF);
    auto payloadSize =
        Builder.CreateZExtOrTrunc(maxPayloadSize(IGF), IGM.Int32Ty);
    auto four = IGM.getInt32(4);
    auto payloadSizeGTE4 = Builder.CreateICmpUGE(payloadSize, four);
    auto whichTag = llvm::PHINode::Create(IGM.Int32Ty, 2);
    auto whichPayloadValue = llvm::PHINode::Create(IGM.Int32Ty, 2);
    auto whichEmptyCase = Builder.CreateSub(tag, numPayloads);
    auto payloadLT4BB = IGF.createBasicBlock("");
    auto storeBB = IGF.createBasicBlock("");
    whichTag->addIncoming(numPayloads, Builder.GetInsertBlock());
    whichPayloadValue->addIncoming(whichEmptyCase, Builder.GetInsertBlock());
    Builder.CreateCondBr(payloadSizeGTE4, storeBB, payloadLT4BB);

    Builder.emitBlock(payloadLT4BB);
    auto numPayloadBits =
        Builder.CreateMul(payloadSize, IGM.getInt32(CHAR_BIT));
    auto tmp = Builder.CreateLShr(whichEmptyCase, numPayloadBits);
    auto tmp2 = Builder.CreateAdd(numPayloads, tmp);
    whichTag->addIncoming(tmp2, Builder.GetInsertBlock());

    auto tmp3 = Builder.CreateSub(
        Builder.CreateShl(IGM.getInt32(1), numPayloadBits), IGM.getInt32(1));
    auto tmp4 = Builder.CreateAnd(whichEmptyCase, tmp3);
    whichPayloadValue->addIncoming(tmp4, Builder.GetInsertBlock());
    Builder.CreateBr(storeBB);

    Builder.emitBlock(storeBB);
    Builder.Insert(whichTag);
    Builder.Insert(whichPayloadValue);
    storeMultiPayloadTag(IGF, whichTag, enumAddr);
    storeMultiPayloadValue(IGF, whichPayloadValue, enumAddr);
  }
  Builder.CreateBr(finishedBB);

  Builder.emitBlock(finishedBB);
}

void EnumTypeLayoutEntry::destructiveInjectEnumTag(IRGenFunction &IGF,
                                                   llvm::Value *tag,
                                                   Address enumAddr) const {
  if (isSingleton()) {
    // No tag, nothing to do
    return;
  } else if (cases.size() == 1) {
    auto payload = cases[0];
    auto emptyCases = IGF.IGM.getInt32(numEmptyCases);
    payload->storeEnumTagSinglePayload(IGF, tag, emptyCases, enumAddr);
    return;
  }

  storeEnumTagMultipayload(IGF, tag, enumAddr);
}

bool EnumTypeLayoutEntry::classof(const TypeLayoutEntry *entry) {
  return entry->kind == TypeLayoutEntryKind::Enum;
}

llvm::Optional<const FixedTypeInfo *>
EnumTypeLayoutEntry::getFixedTypeInfo() const {
  return fixedTypeInfo;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void EnumTypeLayoutEntry::dump() const {
    llvm::dbgs() << "{ enum emptycases: " << numEmptyCases << "\n";
    for (auto *c : cases) {
      c->dump();
    }
    llvm::dbgs() << " id: " << this << " }\n";
}
#endif

void ResilientTypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id) const {
  ResilientTypeLayoutEntry::Profile(id, ty);
}

void ResilientTypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id, SILType ty) {
  id.AddPointer(ty.getASTType().getPointer());
}

ResilientTypeLayoutEntry::~ResilientTypeLayoutEntry() {}

llvm::Value *ResilientTypeLayoutEntry::alignmentMask(IRGenFunction &IGF) const {
  return emitLoadOfAlignmentMask(IGF, ty);
}

llvm::Value *ResilientTypeLayoutEntry::size(IRGenFunction &IGF) const {
  return emitLoadOfSize(IGF, ty);
}

llvm::Optional<Size>
ResilientTypeLayoutEntry::fixedSize(IRGenModule &IGM) const {
  return llvm::None;
}

bool ResilientTypeLayoutEntry::isFixedSize(IRGenModule &IGM) const {
  return false;
}

bool ResilientTypeLayoutEntry::isSingleRetainablePointer() const {
  return false;
}

bool ResilientTypeLayoutEntry::isTriviallyDestroyable() const { return false; }

bool ResilientTypeLayoutEntry::canValueWitnessExtraInhabitantsUpTo(
    IRGenModule &IGM, unsigned index) const {
  return false;
}

llvm::Optional<Alignment>
ResilientTypeLayoutEntry::fixedAlignment(IRGenModule &IGM) const {
  return llvm::None;
}

llvm::Optional<uint32_t>
ResilientTypeLayoutEntry::fixedXICount(IRGenModule &IGM) const {
  return llvm::None;
}

llvm::Value *
ResilientTypeLayoutEntry::extraInhabitantCount(IRGenFunction &IGF) const {
  return emitLoadOfExtraInhabitantCount(IGF, ty);
}

llvm::Value *
ResilientTypeLayoutEntry::isBitwiseTakable(IRGenFunction &IGF) const {
  return emitLoadOfIsBitwiseTakable(IGF, ty);
}

llvm::Constant *
ResilientTypeLayoutEntry::layoutString(IRGenModule &IGM,
                                       GenericSignature genericSig) const {
  return nullptr;
}

bool ResilientTypeLayoutEntry::refCountString(
    IRGenModule &IGM, LayoutStringBuilder &B,
    GenericSignature genericSig) const {
  return false;
}

void ResilientTypeLayoutEntry::computeProperties() {
  hasResilientField = true;
  if (ty.getASTType()->hasArchetype())
    hasDependentResilientField = true;
}

void ResilientTypeLayoutEntry::destroy(IRGenFunction &IGF, Address addr) const {
  emitDestroyCall(IGF, ty, addr);
}

void ResilientTypeLayoutEntry::assignWithCopy(IRGenFunction &IGF, Address dest,
                                              Address src) const {
  emitAssignWithCopyCall(IGF, ty, dest, src);
}

void ResilientTypeLayoutEntry::assignWithTake(IRGenFunction &IGF, Address dest,
                                              Address src) const {
  emitAssignWithTakeCall(IGF, ty, dest, src);
}

void ResilientTypeLayoutEntry::initWithCopy(IRGenFunction &IGF, Address dest,
                                            Address src) const {
  emitInitializeWithCopyCall(IGF, ty, dest, src);
}

void ResilientTypeLayoutEntry::initWithTake(IRGenFunction &IGF, Address dest,
                                            Address src) const {
  emitInitializeWithTakeCall(IGF, ty, dest, src);
}

llvm::Value *ResilientTypeLayoutEntry::getEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *numEmptyCases, Address value) const {

  value = Address(
      IGF.Builder.CreateBitCast(value.getAddress(), IGF.IGM.OpaquePtrTy),
      IGF.IGM.OpaqueTy, value.getAlignment());

  return emitGetEnumTagSinglePayloadCall(IGF, ty, numEmptyCases, value);
}

void ResilientTypeLayoutEntry::storeEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *tag, llvm::Value *numEmptyCases,
    Address addr) const {
  addr =
      Address(IGF.Builder.CreateBitCast(addr.getAddress(), IGF.IGM.OpaquePtrTy),
              IGF.IGM.OpaqueTy, addr.getAlignment());

  emitStoreEnumTagSinglePayloadCall(IGF, ty, tag, numEmptyCases, addr);
}

bool ResilientTypeLayoutEntry::classof(const TypeLayoutEntry *entry) {
  return entry->kind == TypeLayoutEntryKind::Resilient;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void ResilientTypeLayoutEntry::dump() const {
  llvm::dbgs() << "{ resilient type: " << ty << " id: " << this << " }\n";
}
#endif

void TypeInfoBasedTypeLayoutEntry::computeProperties() {
  // does not add anything.
}

void TypeInfoBasedTypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id) const {
  ScalarTypeLayoutEntry::Profile(id, typeInfo, representative);
}

void TypeInfoBasedTypeLayoutEntry::Profile(llvm::FoldingSetNodeID &id,
                                           const TypeInfo &ti, SILType ty) {
  id.AddPointer(&ti);
  id.AddPointer(ty.getASTType().getPointer());
}

TypeInfoBasedTypeLayoutEntry::~TypeInfoBasedTypeLayoutEntry() {}

llvm::Value *
TypeInfoBasedTypeLayoutEntry::alignmentMask(IRGenFunction &IGF) const {
  return typeInfo.getAlignmentMask(IGF, representative);
}

llvm::Value *TypeInfoBasedTypeLayoutEntry::size(IRGenFunction &IGF) const {
  return typeInfo.getSize(IGF, representative);
}

bool TypeInfoBasedTypeLayoutEntry::isFixedSize(IRGenModule &IGM) const {
  return true;
}

llvm::Optional<Size>
TypeInfoBasedTypeLayoutEntry::fixedSize(IRGenModule &IGM) const {
  return typeInfo.getFixedSize();
}

llvm::Optional<Alignment>
TypeInfoBasedTypeLayoutEntry::fixedAlignment(IRGenModule &IGM) const {
  return typeInfo.getFixedAlignment();
}

llvm::Optional<uint32_t>
TypeInfoBasedTypeLayoutEntry::fixedXICount(IRGenModule &IGM) const {
  return typeInfo.getFixedExtraInhabitantCount(IGM);
}

bool TypeInfoBasedTypeLayoutEntry::isTriviallyDestroyable() const {
  return typeInfo.isTriviallyDestroyable(ResilienceExpansion::Maximal);
}

bool TypeInfoBasedTypeLayoutEntry::canValueWitnessExtraInhabitantsUpTo(
    IRGenModule &IGM, unsigned index) const {
  return typeInfo.canValueWitnessExtraInhabitantsUpTo(IGM, index);
}

bool TypeInfoBasedTypeLayoutEntry::isSingleRetainablePointer() const {
  return typeInfo.isSingleRetainablePointer(ResilienceExpansion::Maximal);
}

llvm::Value *
TypeInfoBasedTypeLayoutEntry::extraInhabitantCount(IRGenFunction &IGF) const {
  auto &IGM = IGF.IGM;
  auto fixedXICount =
      cast<FixedTypeInfo>(typeInfo).getFixedExtraInhabitantCount(IGM);
  return llvm::ConstantInt::get(IGM.Int32Ty, fixedXICount);
}

llvm::Value *
TypeInfoBasedTypeLayoutEntry::isBitwiseTakable(IRGenFunction &IGF) const {
  return llvm::ConstantInt::get(IGF.IGM.Int1Ty,
                                cast<FixedTypeInfo>(typeInfo).isBitwiseTakable(
                                    ResilienceExpansion::Maximal));
}

void TypeInfoBasedTypeLayoutEntry::destroy(IRGenFunction &IGF,
                                           Address addr) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto addressType = typeInfo.getStorageType();
  auto *castAddr = IGF.Builder.CreateBitCast(addr.getAddress(),
                                             addressType->getPointerTo());
  addr = Address(castAddr, addressType, alignment);
  typeInfo.destroy(IGF, addr, representative, true);
}

void TypeInfoBasedTypeLayoutEntry::assignWithCopy(IRGenFunction &IGF,
                                                  Address dest,
                                                  Address src) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto addressType = typeInfo.getStorageType();
  auto &Builder = IGF.Builder;
  dest =
      Address(Builder.CreateBitCast(dest.getAddress(),
                                    addressType->getPointerTo()),
              addressType, alignment);
  src =
      Address(Builder.CreateBitCast(src.getAddress(),
                                    addressType->getPointerTo()),
              addressType, alignment);
  typeInfo.assignWithCopy(IGF, dest, src, representative, true);
}

void TypeInfoBasedTypeLayoutEntry::assignWithTake(IRGenFunction &IGF,
                                                  Address dest,
                                                  Address src) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto addressType = typeInfo.getStorageType();
  auto &Builder = IGF.Builder;
  dest =
      Address(Builder.CreateBitCast(dest.getAddress(),
                                    addressType->getPointerTo()),
              addressType, alignment);
  src =
      Address(Builder.CreateBitCast(src.getAddress(),
                                    addressType->getPointerTo()),
              addressType, alignment);
  typeInfo.assignWithTake(IGF, dest, src, representative, true);
}

void TypeInfoBasedTypeLayoutEntry::initWithCopy(IRGenFunction &IGF,
                                                Address dest,
                                                Address src) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto addressType = typeInfo.getStorageType();
  auto &Builder = IGF.Builder;
  dest =
      Address(Builder.CreateBitCast(dest.getAddress(),
                                    addressType->getPointerTo()),
              addressType, alignment);
  src =
      Address(Builder.CreateBitCast(src.getAddress(),
                                    addressType->getPointerTo()),
              addressType, alignment);
  typeInfo.initializeWithCopy(IGF, dest, src, representative, true);
}

void TypeInfoBasedTypeLayoutEntry::initWithTake(IRGenFunction &IGF,
                                                Address dest,
                                                Address src) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto addressType = typeInfo.getStorageType();
  auto &Builder = IGF.Builder;
  dest =
      Address(Builder.CreateBitCast(dest.getAddress(),
                                    addressType->getPointerTo()),
              addressType, alignment);
  src =
      Address(Builder.CreateBitCast(src.getAddress(),
                                    addressType->getPointerTo()),
              addressType, alignment);
  typeInfo.initializeWithTake(IGF, dest, src, representative, true);
}

llvm::Value *TypeInfoBasedTypeLayoutEntry::getEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *numEmptyCases, Address value) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto addressType = typeInfo.getStorageType();
  auto &Builder = IGF.Builder;
  value = Address(Builder.CreateBitCast(value.getAddress(),
                                        addressType->getPointerTo()),
                  addressType, alignment);
  return typeInfo.getEnumTagSinglePayload(IGF, numEmptyCases, value,
                                          representative, true);
}

void TypeInfoBasedTypeLayoutEntry::storeEnumTagSinglePayload(
    IRGenFunction &IGF, llvm::Value *tag, llvm::Value *numEmptyCases,
    Address addr) const {
  auto alignment = typeInfo.getFixedAlignment();
  auto addressType = typeInfo.getStorageType();
  auto &Builder = IGF.Builder;
  addr = Address(Builder.CreateBitCast(addr.getAddress(),
                                       addressType->getPointerTo()),
                 addressType, alignment);
  typeInfo.storeEnumTagSinglePayload(IGF, tag, numEmptyCases, addr,
                                     representative, true);
}

llvm::Constant *
TypeInfoBasedTypeLayoutEntry::layoutString(IRGenModule &IGM,
                                           GenericSignature genericSig) const {
  return nullptr;
}

bool TypeInfoBasedTypeLayoutEntry::refCountString(
    IRGenModule &IGM, LayoutStringBuilder &B,
    GenericSignature genericSig) const {
  return false;
}

llvm::Optional<const FixedTypeInfo *>
TypeInfoBasedTypeLayoutEntry::getFixedTypeInfo() const {
  return &typeInfo;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void TypeInfoBasedTypeLayoutEntry::dump() const {
  if (typeInfo.isFixedSize())
    llvm::dbgs() << "{ scalar (TLB) isFixedSize:" << (bool)typeInfo.isFixedSize()
                 << " isLoadable: " << (bool)typeInfo.isLoadable() << " size: "
                 << cast<FixedTypeInfo>(typeInfo).getFixedSize().getValue()
                 << " alignment: "
                 << cast<FixedTypeInfo>(typeInfo).getFixedAlignment().getValue()
                 << " rep: " << representative << " id: " << this << " }\n";
  if (!typeInfo.isFixedSize()) {
    llvm::dbgs() << "{ scalar non-fixed rep: " << representative
                 << " id: " << this << " }\n";
  }
}
#endif

ScalarTypeLayoutEntry *
TypeLayoutCache::getOrCreateScalarEntry(const TypeInfo &ti,
                                        SILType representative,
                                        ScalarKind kind) {
  assert(ti.isFixedSize());
  llvm::FoldingSetNodeID id;
  ScalarTypeLayoutEntry::Profile(id, cast<FixedTypeInfo>(ti), representative);
  // Do we already have an entry.
  void *insertPos;
  if (auto *entry = scalarEntries.FindNodeOrInsertPos(id, insertPos)) {
    return entry;
  }
  // Otherwise, create a new one.
  auto bytes = sizeof(ScalarTypeLayoutEntry);
  auto mem = bumpAllocator.Allocate(bytes, alignof(ScalarTypeLayoutEntry));
  auto newEntry = new (mem)
      ScalarTypeLayoutEntry(cast<FixedTypeInfo>(ti), representative, kind);
  scalarEntries.InsertNode(newEntry, insertPos);
  newEntry->computeProperties();
  return newEntry;
}

ArchetypeLayoutEntry *
TypeLayoutCache::getOrCreateArchetypeEntry(SILType archetype) {
  llvm::FoldingSetNodeID id;
  ArchetypeLayoutEntry::Profile(id, archetype);
  void *insertPos;
  if (auto *entry = archetypeEntries.FindNodeOrInsertPos(id, insertPos)) {
    return entry;
  }
  auto bytes = sizeof(ArchetypeLayoutEntry);
  auto mem = bumpAllocator.Allocate(bytes, alignof(ArchetypeLayoutEntry));
  auto newEntry = new (mem) ArchetypeLayoutEntry(archetype);
  archetypeEntries.InsertNode(newEntry, insertPos);
  newEntry->computeProperties();
  return newEntry;
}

AlignedGroupEntry *TypeLayoutCache::getOrCreateAlignedGroupEntry(
    const std::vector<TypeLayoutEntry *> &entries, SILType ty,
    Alignment::int_type minimumAlignment, const TypeInfo &ti) {
  llvm::FoldingSetNodeID id;
  AlignedGroupEntry::Profile(id, entries, minimumAlignment);
  void *insertPos;
  if (auto *entry = alignedGroupEntries.FindNodeOrInsertPos(id, insertPos)) {
    return entry;
  }
  auto bytes = sizeof(AlignedGroupEntry);
  auto mem = bumpAllocator.Allocate(bytes, alignof(AlignedGroupEntry));

  llvm::Optional<const FixedTypeInfo *> fixedTypeInfo;
  if (const auto *fixedTI = dyn_cast<const FixedTypeInfo>(&ti)) {
    fixedTypeInfo = fixedTI;
  }

  auto newEntry =
      new (mem) AlignedGroupEntry(entries, ty, minimumAlignment, fixedTypeInfo);
  alignedGroupEntries.InsertNode(newEntry, insertPos);
  newEntry->computeProperties();
  return newEntry;
}

TypeLayoutEntry *TypeLayoutCache::getEmptyEntry() { return &emptyEntry; }

EnumTypeLayoutEntry *TypeLayoutCache::getOrCreateEnumEntry(
    unsigned numEmptyCases, const std::vector<TypeLayoutEntry *> &nonEmptyCases,
    SILType ty, const TypeInfo &ti) {

  llvm::FoldingSetNodeID id;
  EnumTypeLayoutEntry::Profile(id, numEmptyCases, nonEmptyCases);
  void *insertPos;
  if (auto *entry = enumEntries.FindNodeOrInsertPos(id, insertPos)) {
    return entry;
  }
  auto bytes = sizeof(EnumTypeLayoutEntry);
  auto mem = bumpAllocator.Allocate(bytes, alignof(EnumTypeLayoutEntry));

  llvm::Optional<Size> fixedSize;
  llvm::Optional<const FixedTypeInfo *> fixedTypeInfo;
  if (const auto *fixedTI = dyn_cast<const FixedTypeInfo>(&ti)) {
    fixedSize = fixedTI->getFixedSize();
    fixedTypeInfo = fixedTI;
  }

  auto newEntry = new (mem)
      EnumTypeLayoutEntry(numEmptyCases, nonEmptyCases, ty, fixedTypeInfo,
                          ti.getBestKnownAlignment().getValue(), fixedSize);
  enumEntries.InsertNode(newEntry, insertPos);
  newEntry->computeProperties();
  return newEntry;
}

ResilientTypeLayoutEntry *
TypeLayoutCache::getOrCreateResilientEntry(SILType ty) {
  llvm::FoldingSetNodeID id;
  ResilientTypeLayoutEntry::Profile(id, ty);
  void *insertPos;
  if (auto *entry = resilientEntries.FindNodeOrInsertPos(id, insertPos)) {
    return entry;
  }
  auto bytes = sizeof(ResilientTypeLayoutEntry);
  auto mem = bumpAllocator.Allocate(bytes, alignof(ResilientTypeLayoutEntry));
  auto newEntry = new (mem) ResilientTypeLayoutEntry(ty);
  resilientEntries.InsertNode(newEntry, insertPos);
  newEntry->computeProperties();
  return newEntry;
}

TypeInfoBasedTypeLayoutEntry *
TypeLayoutCache::getOrCreateTypeInfoBasedEntry(const TypeInfo &ti,
                                               SILType representative) {
  assert(ti.isFixedSize());
  llvm::FoldingSetNodeID id;
  TypeInfoBasedTypeLayoutEntry::Profile(id, ti, representative);
  // Grab the entry from the cache if we have one
  void *insertPos;
  if (auto *entry = typeInfoBasedEntries.FindNodeOrInsertPos(id, insertPos)) {
    return entry;
  }
  // Otherwise, create a new one.
  const auto bytes = sizeof(ScalarTypeLayoutEntry);
  auto mem = bumpAllocator.Allocate(bytes, alignof(ScalarTypeLayoutEntry));
  auto newEntry = new (mem)
      TypeInfoBasedTypeLayoutEntry(cast<FixedTypeInfo>(ti), representative);
  typeInfoBasedEntries.InsertNode(newEntry, insertPos);
  newEntry->computeProperties();
  return newEntry;
}

TypeLayoutCache::~TypeLayoutCache() {
  for (auto &entry : scalarEntries) {
    entry.~ScalarTypeLayoutEntry();
  }
  for (auto &entry : archetypeEntries) {
    entry.~ArchetypeLayoutEntry();
  }
  for (auto &entry : alignedGroupEntries) {
    entry.~AlignedGroupEntry();
  }
  for (auto &entry : enumEntries) {
    entry.~EnumTypeLayoutEntry();
  }
  for (auto &entry : resilientEntries) {
    entry.~ResilientTypeLayoutEntry();
  }
  for (auto &entry : typeInfoBasedEntries) {
    entry.~TypeInfoBasedTypeLayoutEntry();
  }
}

//===--- SwiftRemoteMirror.cpp - C wrapper for Reflection API -------------===//
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

#include "swift/SwiftRemoteMirror/Platform.h"
#include "swift/SwiftRemoteMirror/SwiftRemoteMirror.h"
#include <iostream>
#include <variant>

#define SWIFT_CLASS_IS_SWIFT_MASK swift_reflection_classIsSwiftMask
extern "C" {
SWIFT_REMOTE_MIRROR_LINKAGE
unsigned long long swift_reflection_classIsSwiftMask = 2;

SWIFT_REMOTE_MIRROR_LINKAGE uint32_t swift_reflection_libraryVersion = 3;
}

#include "swift/Demangling/Demangler.h"
#include "swift/RemoteInspection/ReflectionContext.h"
#include "swift/RemoteInspection/TypeLowering.h"
#include "swift/Remote/CMemoryReader.h"
#include "swift/Basic/Unreachable.h"

#if defined(__APPLE__) && defined(__MACH__)
#include <TargetConditionals.h>
#endif

using namespace swift;
using namespace swift::reflection;
using namespace swift::remote;

using RuntimeWithObjCInterop =
    External<WithObjCInterop<RuntimeTarget<sizeof(uintptr_t)>>>;
using RuntimeNoObjCInterop =
    External<NoObjCInterop<RuntimeTarget<sizeof(uintptr_t)>>>;

using ReflectionContextWithObjCInterop =
    swift::reflection::ReflectionContext<RuntimeWithObjCInterop>;
using ReflectionContextNoObjCInterop =
    swift::reflection::ReflectionContext<RuntimeNoObjCInterop>;

struct SwiftReflectionContext {
  using ContextVariant =
      std::variant<std::unique_ptr<ReflectionContextWithObjCInterop>,
                   std::unique_ptr<ReflectionContextNoObjCInterop>>;

  ContextVariant context;
  std::vector<std::function<void()>> freeFuncs;
  std::vector<std::tuple<swift_addr_t, swift_addr_t>> dataSegments;

  std::function<void(void)> freeTemporaryAllocation = [] {};

  SwiftReflectionContext(bool objCInteropIsEnabled, MemoryReaderImpl impl) {
    auto Reader = std::make_shared<CMemoryReader>(impl);
    if (objCInteropIsEnabled) {
      context = std::make_unique<ReflectionContextWithObjCInterop>(Reader);
    } else {
      context = std::make_unique<ReflectionContextNoObjCInterop>(Reader);
    }
  }

  ~SwiftReflectionContext() {
    freeTemporaryAllocation();
    for (auto f : freeFuncs)
      f();
  }

  // Allocate a single temporary object that will stay allocated until the next
  // call to this method, or until the context is destroyed.
  template <typename T>
  T *allocateTemporaryObject() {
    freeTemporaryAllocation();
    T *obj = new T;
    freeTemporaryAllocation = [obj] { delete obj; };
    return obj;
  }

  // Allocate a single temporary object that will stay allocated until the next
  // call to allocateTemporaryObject, or until the context is destroyed. Does
  // NOT free any existing objects created with allocateTemporaryObject or
  // allocateSubsequentTemporaryObject. Use to allocate additional objects after
  // a call to allocateTemporaryObject when multiple objects are needed
  // simultaneously.
  template <typename T>
  T *allocateSubsequentTemporaryObject() {
    T *obj = new T;
    auto oldFree = freeTemporaryAllocation;
    freeTemporaryAllocation = [obj, oldFree] {
      delete obj;
      oldFree();
    };
    return obj;
  }

  // Call fn with a pointer to context.
  template <typename T, typename Fn>
  T withContext(const Fn &fn) {
    return std::visit([&](auto &&context) { return fn(context.get()); },
                      this->context);
  }
};

uint16_t
swift_reflection_getSupportedMetadataVersion() {
  return SWIFT_REFLECTION_METADATA_VERSION;
}

template <uint8_t WordSize>
static int minimalDataLayoutQueryFunction(void *ReaderContext,
                                          DataLayoutQueryType type,
                                          void *inBuffer, void *outBuffer) {
    // TODO: The following should be set based on the target.
    // This code sets it to match the platform this code was compiled for.
#if defined(__APPLE__) && __APPLE__
    auto applePlatform = true;
#else
    auto applePlatform = false;
#endif
#if defined(__APPLE__) && __APPLE__ && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_IOS) && TARGET_OS_WATCH) || (defined(TARGET_OS_TV) && TARGET_OS_TV) || defined(__arm64__))
    auto iosDerivedPlatform = true;
#else
    auto iosDerivedPlatform = false;
#endif

  if (type == DLQ_GetPointerSize || type == DLQ_GetSizeSize) {
    auto result = static_cast<uint8_t *>(outBuffer);
    *result = WordSize;
    return 1;
  }
  if (type == DLQ_GetObjCReservedLowBits) {
    auto result = static_cast<uint8_t *>(outBuffer);
    if (applePlatform && !iosDerivedPlatform && WordSize == 8) {
      // Obj-C reserves low bit on 64-bit macOS only.
      // Other Apple platforms don't reserve this bit (even when
      // running on x86_64-based simulators).
      *result = 1;
    } else {
      *result = 0;
    }
    return 1;
  }
  if (type == DLQ_GetLeastValidPointerValue) {
    auto result = static_cast<uint64_t *>(outBuffer);
    if (applePlatform && WordSize == 8) {
      // Swift reserves the first 4GiB on all 64-bit Apple platforms
      *result = 0x100000000;
    } else {
      // Swift reserves the first 4KiB everywhere else
      *result = 0x1000;
    }
    return 1;
  }
  return 0;
}

// Caveat: This basically only works correctly if running on the same
// host as the target.  Otherwise, you'll need to use
// swift_reflection_createReflectionContextWithDataLayout() below
// with an appropriate data layout query function that understands
// the target environment.
SwiftReflectionContextRef
swift_reflection_createReflectionContext(void *ReaderContext,
                                         uint8_t PointerSize,
                                         FreeBytesFunction Free,
                                         ReadBytesFunction ReadBytes,
                                         GetStringLengthFunction GetStringLength,
                                         GetSymbolAddressFunction GetSymbolAddress) {
  assert((PointerSize == 4 || PointerSize == 8) && "We only support 32-bit and 64-bit.");
  assert(PointerSize == sizeof(uintptr_t) &&
         "We currently only support the pointer size this file was compiled with.");

  auto *DataLayout = PointerSize == 4 ? minimalDataLayoutQueryFunction<4>
                                      : minimalDataLayoutQueryFunction<8>;
  MemoryReaderImpl ReaderImpl {
    ReaderContext,
    DataLayout,
    Free,
    ReadBytes,
    GetStringLength,
    GetSymbolAddress
  };

  return new SwiftReflectionContext(SWIFT_OBJC_INTEROP, ReaderImpl);
}

SwiftReflectionContextRef
swift_reflection_createReflectionContextWithDataLayout(void *ReaderContext,
                                    QueryDataLayoutFunction DataLayout,
                                    FreeBytesFunction Free,
                                    ReadBytesFunction ReadBytes,
                                    GetStringLengthFunction GetStringLength,
                                    GetSymbolAddressFunction GetSymbolAddress) {
  MemoryReaderImpl ReaderImpl {
    ReaderContext,
    DataLayout,
    Free,
    ReadBytes,
    GetStringLength,
    GetSymbolAddress
  };

  // If the client implements DLQ_GetObjCInteropIsEnabled, use that value.
  // If they don't, use this platform's default.
  bool dataLayoutSaysObjCInteropIsEnabled = true;
  if (DataLayout(ReaderContext, DLQ_GetObjCInteropIsEnabled, nullptr,
                 (void *)&dataLayoutSaysObjCInteropIsEnabled)) {
    return new SwiftReflectionContext(dataLayoutSaysObjCInteropIsEnabled,
                                      ReaderImpl);
  } else {
    return new SwiftReflectionContext(SWIFT_OBJC_INTEROP, ReaderImpl);
  }
}

void swift_reflection_destroyReflectionContext(SwiftReflectionContextRef ContextRef) {
  delete ContextRef;
}

template<typename Iterator>
ReflectionSection<Iterator> sectionFromInfo(const swift_reflection_info_t &Info,
                              const swift_reflection_section_pair_t &Section) {
  auto RemoteSectionStart = (uint64_t)(uintptr_t)Section.section.Begin
    - Info.LocalStartAddress
    + Info.RemoteStartAddress;

  auto Start = RemoteRef<void>(RemoteSectionStart, Section.section.Begin);

  return ReflectionSection<Iterator>(Start,
             (uintptr_t)Section.section.End - (uintptr_t)Section.section.Begin);
}

template <typename Iterator>
ReflectionSection<Iterator> reflectionSectionFromLocalAndRemote(
    const swift_reflection_section_mapping_t &Section) {
  auto RemoteSectionStart = (uint64_t)Section.remote_section.StartAddress;

  auto Start = RemoteRef<void>(RemoteSectionStart, Section.local_section.Begin);

  return ReflectionSection<Iterator>(Start,
                                     (uintptr_t)Section.remote_section.Size);
}

void
swift_reflection_addReflectionInfo(SwiftReflectionContextRef ContextRef,
                                   swift_reflection_info_t Info) {
  ContextRef->withContext<void>([&](auto *Context) {
    // The `offset` fields must be zero.
    if (Info.field.offset != 0
        || Info.associated_types.offset != 0
        || Info.builtin_types.offset != 0
        || Info.capture.offset != 0
        || Info.type_references.offset != 0
        || Info.reflection_strings.offset != 0) {
      std::cerr << "reserved field in swift_reflection_info_t is not zero\n";
      abort();
    }

    ReflectionInfo ContextInfo{
        sectionFromInfo<FieldDescriptorIterator>(Info, Info.field),
        sectionFromInfo<AssociatedTypeIterator>(Info, Info.associated_types),
        sectionFromInfo<BuiltinTypeDescriptorIterator>(Info,
                                                       Info.builtin_types),
        sectionFromInfo<CaptureDescriptorIterator>(Info, Info.capture),
        sectionFromInfo<const void *>(Info, Info.type_references),
        sectionFromInfo<const void *>(Info, Info.reflection_strings),
        ReflectionSection<const void *>(nullptr, 0),
        {}};

    Context->addReflectionInfo(ContextInfo);
  });
}

void swift_reflection_addReflectionMappingInfo(
    SwiftReflectionContextRef ContextRef,
    swift_reflection_mapping_info_t Info) {
  return ContextRef->withContext<void>([&](auto *Context) {
    ReflectionInfo ContextInfo{
        reflectionSectionFromLocalAndRemote<FieldDescriptorIterator>(
            Info.field),
        reflectionSectionFromLocalAndRemote<AssociatedTypeIterator>(
            Info.associated_types),
        reflectionSectionFromLocalAndRemote<BuiltinTypeDescriptorIterator>(
            Info.builtin_types),
        reflectionSectionFromLocalAndRemote<CaptureDescriptorIterator>(
            Info.capture),
        reflectionSectionFromLocalAndRemote<const void *>(Info.type_references),
        reflectionSectionFromLocalAndRemote<const void *>(
            Info.reflection_strings),
        ReflectionSection<const void *>(nullptr, 0),
        {}};

    Context->addReflectionInfo(ContextInfo);
  });
}

int
swift_reflection_addImage(SwiftReflectionContextRef ContextRef,
                          swift_addr_t imageStart) {
  return ContextRef->withContext<int>([&](auto *Context) {
    return Context->addImage(RemoteAddress(imageStart)).has_value();
  });
}

int
swift_reflection_readIsaMask(SwiftReflectionContextRef ContextRef,
                             uintptr_t *outIsaMask) {
  return ContextRef->withContext<int>([&](auto *Context) {
    auto isaMask = Context->readIsaMask();
    if (isaMask) {
      *outIsaMask = *isaMask;
      return true;
    }
    *outIsaMask = 0;
    return false;
  });
}

swift_typeref_t
swift_reflection_typeRefForMetadata(SwiftReflectionContextRef ContextRef,
                                    uintptr_t Metadata) {
  return ContextRef->withContext<swift_typeref_t>([&](auto *Context) {
    auto TR = Context->readTypeFromMetadata(Metadata);
    return reinterpret_cast<swift_typeref_t>(TR);
  });
}

int
swift_reflection_ownsObject(SwiftReflectionContextRef ContextRef, uintptr_t Object) {
  return ContextRef->withContext<int>([&](auto *Context) {
    return Context->ownsObject(RemoteAddress(Object));
  });
}

int
swift_reflection_ownsAddress(SwiftReflectionContextRef ContextRef, uintptr_t Address) {
  return ContextRef->withContext<int>([&](auto *Context) {
    return Context->ownsAddress(RemoteAddress(Address));
  });
}

int
swift_reflection_ownsAddressStrict(SwiftReflectionContextRef ContextRef, uintptr_t Address) {
  return ContextRef->withContext<int>([&](auto *Context) {
    return Context->ownsAddress(RemoteAddress(Address), false);
  });
}

uintptr_t
swift_reflection_metadataForObject(SwiftReflectionContextRef ContextRef,
                                   uintptr_t Object) {
  return ContextRef->withContext<uintptr_t>([&](auto *Context) -> uintptr_t {
    auto MetadataAddress = Context->readMetadataFromInstance(Object);
    if (!MetadataAddress)
      return 0;
    return *MetadataAddress;
  });
}

swift_reflection_ptr_t
swift_reflection_metadataNominalTypeDescriptor(SwiftReflectionContextRef ContextRef,
                                               swift_reflection_ptr_t MetadataAddress) {
  return ContextRef->withContext<swift_reflection_ptr_t>([&](auto *Context) {
    return Context->nominalTypeDescriptorFromMetadata(MetadataAddress);
  });
}

int swift_reflection_metadataIsActor(SwiftReflectionContextRef ContextRef,
                                     swift_reflection_ptr_t Metadata) {
  return ContextRef->withContext<int>(
      [&](auto *Context) { return Context->metadataIsActor(Metadata); });
}

swift_typeref_t
swift_reflection_typeRefForInstance(SwiftReflectionContextRef ContextRef,
                                    uintptr_t Object) {
  return ContextRef->withContext<swift_typeref_t>(
      [&](auto *Context) -> swift_typeref_t {
        auto MetadataAddress = Context->readMetadataFromInstance(Object);
        if (!MetadataAddress)
          return 0;
        auto TR = Context->readTypeFromMetadata(*MetadataAddress);
        return reinterpret_cast<swift_typeref_t>(TR);
      });
}

swift_typeref_t
swift_reflection_typeRefForMangledTypeName(SwiftReflectionContextRef ContextRef,
                                           const char *MangledTypeName,
                                           uint64_t Length) {
  return ContextRef->withContext<swift_typeref_t>([&](auto *Context) {
    auto TR =
        Context->readTypeFromMangledName(MangledTypeName, Length).getType();
    return reinterpret_cast<swift_typeref_t>(TR);
  });
}

char *
swift_reflection_copyDemangledNameForTypeRef(
  SwiftReflectionContextRef ContextRef, swift_typeref_t OpaqueTypeRef) {
  auto TR = reinterpret_cast<const TypeRef *>(OpaqueTypeRef);

  Demangle::Demangler Dem;
  auto Name = nodeToString(TR->getDemangling(Dem));
  return strdup(Name.c_str());
}

char *
swift_reflection_copyNameForTypeRef(SwiftReflectionContextRef ContextRef,
                                    swift_typeref_t OpaqueTypeRef,
                                    bool mangled) {
  auto TR = reinterpret_cast<const TypeRef *>(OpaqueTypeRef);

  Demangle::Demangler Dem;
  if (mangled) {
    auto Mangling = mangleNode(TR->getDemangling(Dem));
    if (Mangling.isSuccess()) {
      return strdup(Mangling.result().c_str());
    }
  }
  else {
    auto Name = nodeToString(TR->getDemangling(Dem));
    return strdup(Name.c_str());
  }
  return nullptr;
}

SWIFT_REMOTE_MIRROR_LINKAGE
char *
swift_reflection_copyDemangledNameForProtocolDescriptor(
  SwiftReflectionContextRef ContextRef, swift_reflection_ptr_t Proto) {
  return ContextRef->withContext<char *>([&](auto *Context) {

    Demangle::Demangler Dem;
    auto Demangling = Context->readDemanglingForContextDescriptor(Proto, Dem);
    auto Name = nodeToString(Demangling);
    return strdup(Name.c_str());
  });
}

swift_typeref_t
swift_reflection_genericArgumentOfTypeRef(swift_typeref_t OpaqueTypeRef,
                                          unsigned Index) {
  auto TR = reinterpret_cast<const TypeRef *>(OpaqueTypeRef);

  if (auto BG = dyn_cast<BoundGenericTypeRef>(TR)) {
    auto &Params = BG->getGenericParams();
    assert(Index < Params.size());
    return reinterpret_cast<swift_typeref_t>(Params[Index]);
  }
  return 0;
}

unsigned
swift_reflection_genericArgumentCountOfTypeRef(swift_typeref_t OpaqueTypeRef) {
  auto TR = reinterpret_cast<const TypeRef *>(OpaqueTypeRef);

  if (auto BG = dyn_cast<BoundGenericTypeRef>(TR)) {
    auto &Params = BG->getGenericParams();
    return Params.size();
  }
  return 0;
}

swift_layout_kind_t getTypeInfoKind(const TypeInfo &TI) {
  switch (TI.getKind()) {
  case TypeInfoKind::Invalid: {
    return SWIFT_UNKNOWN;
  }
  case TypeInfoKind::Builtin: {
    auto &BuiltinTI = cast<BuiltinTypeInfo>(TI);
    if (BuiltinTI.getMangledTypeName() == "Bp")
      return SWIFT_RAW_POINTER;
    return SWIFT_BUILTIN;
  }
  case TypeInfoKind::Record: {
    auto &RecordTI = cast<RecordTypeInfo>(TI);
    switch (RecordTI.getRecordKind()) {
    case RecordKind::Invalid:
      return SWIFT_UNKNOWN;
    case RecordKind::Tuple:
      return SWIFT_TUPLE;
    case RecordKind::Struct:
      return SWIFT_STRUCT;
    case RecordKind::ThickFunction:
      return SWIFT_THICK_FUNCTION;
    case RecordKind::OpaqueExistential:
      return SWIFT_OPAQUE_EXISTENTIAL;
    case RecordKind::ClassExistential:
      return SWIFT_CLASS_EXISTENTIAL;
    case RecordKind::ErrorExistential:
      return SWIFT_ERROR_EXISTENTIAL;
    case RecordKind::ExistentialMetatype:
      return SWIFT_EXISTENTIAL_METATYPE;
    case RecordKind::ClassInstance:
      return SWIFT_CLASS_INSTANCE;
    case RecordKind::ClosureContext:
      return SWIFT_CLOSURE_CONTEXT;
    }
  }
  case TypeInfoKind::Enum: {
    auto &EnumTI = cast<EnumTypeInfo>(TI);
    switch (EnumTI.getEnumKind()) {
    case EnumKind::NoPayloadEnum:
      return SWIFT_NO_PAYLOAD_ENUM;
    case EnumKind::SinglePayloadEnum:
      return SWIFT_SINGLE_PAYLOAD_ENUM;
    case EnumKind::MultiPayloadEnum:
      return SWIFT_MULTI_PAYLOAD_ENUM;
    }
  }
  case TypeInfoKind::Reference: {
    auto &ReferenceTI = cast<ReferenceTypeInfo>(TI);
    switch (ReferenceTI.getReferenceKind()) {
    case ReferenceKind::Strong: return SWIFT_STRONG_REFERENCE;
#define REF_STORAGE(Name, name, NAME) \
    case ReferenceKind::Name: return SWIFT_##NAME##_REFERENCE;
#include "swift/AST/ReferenceStorage.def"
    }
  }
  }

  swift_unreachable("Unhandled TypeInfoKind in switch");
}

static swift_typeinfo_t convertTypeInfo(const TypeInfo *TI) {
  if (TI == nullptr) {
    return {
      SWIFT_UNKNOWN,
      0,
      0,
      0,
      0
    };
  }

  unsigned NumFields = 0;
  if (auto *RecordTI = dyn_cast<EnumTypeInfo>(TI)) {
    NumFields = RecordTI->getNumCases();
  } else if (auto *RecordTI = dyn_cast<RecordTypeInfo>(TI)) {
    NumFields = RecordTI->getNumFields();
  }

  return {
    getTypeInfoKind(*TI),
    TI->getSize(),
    TI->getAlignment(),
    TI->getStride(),
    NumFields
  };
}

static swift_childinfo_t convertChild(const TypeInfo *TI, unsigned Index) {
  if (!TI)
    return {};

  const FieldInfo *FieldInfo = nullptr;
  if (auto *EnumTI = dyn_cast<EnumTypeInfo>(TI)) {
    FieldInfo = &(EnumTI->getCases()[Index]);
  } else if (auto *RecordTI = dyn_cast<RecordTypeInfo>(TI)) {
    FieldInfo = &(RecordTI->getFields()[Index]);
  } else {
    assert(false && "convertChild(TI): TI must be record or enum typeinfo");
    return {
      "unknown TypeInfo kind",
      0,
      SWIFT_UNKNOWN,
      0,
    };
  }

  return {
    FieldInfo->Name.c_str(),
    FieldInfo->Offset,
    getTypeInfoKind(FieldInfo->TI),
    reinterpret_cast<swift_typeref_t>(FieldInfo->TR),
  };
}

template <typename ReflectionContext>
static swift_layout_kind_t convertAllocationChunkKind(
    typename ReflectionContext::AsyncTaskAllocationChunk::ChunkKind Kind) {
  switch (Kind) {
  case ReflectionContext::AsyncTaskAllocationChunk::ChunkKind::Unknown:
    return SWIFT_UNKNOWN;
  case ReflectionContext::AsyncTaskAllocationChunk::ChunkKind::NonPointer:
    return SWIFT_BUILTIN;
  case ReflectionContext::AsyncTaskAllocationChunk::ChunkKind::RawPointer:
    return SWIFT_RAW_POINTER;
  case ReflectionContext::AsyncTaskAllocationChunk::ChunkKind::StrongReference:
    return SWIFT_STRONG_REFERENCE;
  case ReflectionContext::AsyncTaskAllocationChunk::ChunkKind::UnownedReference:
    return SWIFT_UNOWNED_REFERENCE;
  case ReflectionContext::AsyncTaskAllocationChunk::ChunkKind::WeakReference:
    return SWIFT_WEAK_REFERENCE;
  case ReflectionContext::AsyncTaskAllocationChunk::ChunkKind::
      UnmanagedReference:
    return SWIFT_UNMANAGED_REFERENCE;
  }
}

static const char *returnableCString(SwiftReflectionContextRef ContextRef,
                                     std::optional<std::string> String) {
  if (String) {
    auto *TmpStr = ContextRef->allocateTemporaryObject<std::string>();
    *TmpStr = *String;
    return TmpStr->c_str();
  }
  return nullptr;
}

swift_typeinfo_t
swift_reflection_infoForTypeRef(SwiftReflectionContextRef ContextRef,
                                swift_typeref_t OpaqueTypeRef) {
  return ContextRef->withContext<swift_typeinfo_t>([&](auto *Context) {
    auto TR = reinterpret_cast<const TypeRef *>(OpaqueTypeRef);
    auto TI = Context->getTypeInfo(TR, nullptr);
    return convertTypeInfo(TI);
  });
}

swift_childinfo_t
swift_reflection_childOfTypeRef(SwiftReflectionContextRef ContextRef,
                                swift_typeref_t OpaqueTypeRef,
                                unsigned Index) {
  return ContextRef->withContext<swift_childinfo_t>([&](auto *Context) {
    auto TR = reinterpret_cast<const TypeRef *>(OpaqueTypeRef);
    auto *TI = Context->getTypeInfo(TR, nullptr);
    return convertChild(TI, Index);
  });
}

swift_typeinfo_t
swift_reflection_infoForMetadata(SwiftReflectionContextRef ContextRef,
                                 uintptr_t Metadata) {
  return ContextRef->withContext<swift_typeinfo_t>([&](auto *Context) {
    auto *TI = Context->getMetadataTypeInfo(Metadata, nullptr);
    return convertTypeInfo(TI);
  });
}

swift_childinfo_t
swift_reflection_childOfMetadata(SwiftReflectionContextRef ContextRef,
                                 uintptr_t Metadata,
                                 unsigned Index) {
  return ContextRef->withContext<swift_childinfo_t>([&](auto *Context) {
    auto *TI = Context->getMetadataTypeInfo(Metadata, nullptr);
    return convertChild(TI, Index);
  });
}

swift_typeinfo_t
swift_reflection_infoForInstance(SwiftReflectionContextRef ContextRef,
                                 uintptr_t Object) {
  return ContextRef->withContext<swift_typeinfo_t>([&](auto *Context) {
    auto *TI = Context->getInstanceTypeInfo(Object, nullptr);
    return convertTypeInfo(TI);
  });
}

swift_childinfo_t
swift_reflection_childOfInstance(SwiftReflectionContextRef ContextRef,
                                 uintptr_t Object,
                                 unsigned Index) {
  return ContextRef->withContext<swift_childinfo_t>([&](auto *Context) {
    auto *TI = Context->getInstanceTypeInfo(Object, nullptr);
    return convertChild(TI, Index);
  });
}

int swift_reflection_projectExistential(SwiftReflectionContextRef ContextRef,
                                        swift_addr_t ExistentialAddress,
                                        swift_typeref_t ExistentialTypeRef,
                                        swift_typeref_t *InstanceTypeRef,
                                        swift_addr_t *StartOfInstanceData) {
  return ContextRef->withContext<int>([&](auto *Context) {
    auto ExistentialTR = reinterpret_cast<const TypeRef *>(ExistentialTypeRef);
    auto RemoteExistentialAddress = RemoteAddress(ExistentialAddress);
    const TypeRef *InstanceTR = nullptr;
    RemoteAddress RemoteStartOfInstanceData(nullptr);
    auto Success = Context->projectExistential(
        RemoteExistentialAddress, ExistentialTR, &InstanceTR,
        &RemoteStartOfInstanceData, nullptr);

    if (Success) {
      *InstanceTypeRef = reinterpret_cast<swift_typeref_t>(InstanceTR);
      *StartOfInstanceData = RemoteStartOfInstanceData.getAddressData();
    }

    return Success;
  });
}

int swift_reflection_projectExistentialAndUnwrapClass(SwiftReflectionContextRef ContextRef,
                                        swift_addr_t ExistentialAddress,
                                        swift_typeref_t ExistentialTypeRef,
                                        swift_typeref_t *InstanceTypeRef,
                                        swift_addr_t *StartOfInstanceData) {
  return ContextRef->withContext<int>([&](auto *Context) {
    auto ExistentialTR = reinterpret_cast<const TypeRef *>(ExistentialTypeRef);
    auto RemoteExistentialAddress = RemoteAddress(ExistentialAddress);
    auto Pair = Context->projectExistentialAndUnwrapClass(
        RemoteExistentialAddress, *ExistentialTR);
    if (!Pair.has_value())
      return false;
    *InstanceTypeRef =
        reinterpret_cast<swift_typeref_t>(std::get<const TypeRef *>(*Pair));
    *StartOfInstanceData = std::get<RemoteAddress>(*Pair).getAddressData();

    return true;
  });
}

int swift_reflection_projectEnumValue(SwiftReflectionContextRef ContextRef,
                                      swift_addr_t EnumAddress,
                                      swift_typeref_t EnumTypeRef,
                                      int *CaseIndex) {
  return ContextRef->withContext<int>([&](auto *Context) {
    auto EnumTR = reinterpret_cast<const TypeRef *>(EnumTypeRef);
    auto RemoteEnumAddress = RemoteAddress(EnumAddress);
    if (!Context->projectEnumValue(RemoteEnumAddress, EnumTR, CaseIndex,
                                   nullptr)) {
      return false;
    }
    auto TI = Context->getTypeInfo(EnumTR, nullptr);
    auto *RecordTI = dyn_cast<EnumTypeInfo>(TI);
    assert(RecordTI != nullptr);
    if (static_cast<size_t>(*CaseIndex) >= RecordTI->getNumCases()) {
      return false;
    }
    return true;
  });
}

void swift_reflection_dumpTypeRef(swift_typeref_t OpaqueTypeRef) {
  auto TR = reinterpret_cast<const TypeRef *>(OpaqueTypeRef);
  if (TR == nullptr) {
    std::cout << "<null type reference>\n";
  } else {
    TR->dump(std::cout);
  }
}

void swift_reflection_dumpInfoForTypeRef(SwiftReflectionContextRef ContextRef,
                                         swift_typeref_t OpaqueTypeRef) {
  ContextRef->withContext<void>([&](auto *Context) {
    auto TR = reinterpret_cast<const TypeRef *>(OpaqueTypeRef);
    auto TI = Context->getTypeInfo(TR, nullptr);
    if (TI == nullptr) {
      std::cout << "<null type info>\n";
    } else {
      TI->dump(std::cout);
      Demangle::Demangler Dem;
      auto Mangling = mangleNode(TR->getDemangling(Dem));
      std::string MangledName;
      if (Mangling.isSuccess()) {
        MangledName = Mangling.result();
        std::cout << "Mangled name: " << MANGLING_PREFIX_STR << MangledName
                  << "\n";
      } else {
        MangledName = "<failed to mangle name>";
        std::cout 
          << "Failed to get mangled name: Node " << Mangling.error().node
          << " error " << Mangling.error().code << ":"
          << Mangling.error().line << "\n";
      }

      char *DemangledName =
          swift_reflection_copyNameForTypeRef(ContextRef, OpaqueTypeRef, false);
      std::cout << "Demangled name: " << DemangledName << "\n";
      free(DemangledName);
    }
  });
}

void swift_reflection_dumpInfoForMetadata(SwiftReflectionContextRef ContextRef,
                                          uintptr_t Metadata) {
  ContextRef->withContext<void>([&](auto *Context) {
    auto TI = Context->getMetadataTypeInfo(Metadata, nullptr);
    if (TI == nullptr) {
      std::cout << "<null type info>\n";
    } else {
      TI->dump(std::cout);
    }
  });
}

void swift_reflection_dumpInfoForInstance(SwiftReflectionContextRef ContextRef,
                                          uintptr_t Object) {
  ContextRef->withContext<void>([&](auto *Context) {
    auto TI = Context->getInstanceTypeInfo(Object, nullptr);
    if (TI == nullptr) {
      std::cout << "<null type info>\n";
    } else {
      TI->dump(std::cout);
    }
  });
}

size_t swift_reflection_demangle(const char *MangledName, size_t Length,
                                 char *OutDemangledName, size_t MaxLength) {
  if (MangledName == nullptr || Length == 0)
    return 0;

  std::string Mangled(MangledName, Length);
  auto Demangled = Demangle::demangleTypeAsString(Mangled);
  strncpy(OutDemangledName, Demangled.c_str(), MaxLength);
  return Demangled.size();
}

const char *swift_reflection_iterateConformanceCache(
  SwiftReflectionContextRef ContextRef,
  void (*Call)(swift_reflection_ptr_t Type,
               swift_reflection_ptr_t Proto,
               void *ContextPtr),
  void *ContextPtr) {
  return ContextRef->withContext<const char *>([&](auto *Context) {
    auto Error = Context->iterateConformances([&](auto Type, auto Proto) {
      Call(Type, Proto, ContextPtr);
    });
    return returnableCString(ContextRef, Error);
  });
}

const char *swift_reflection_iterateMetadataAllocations(
  SwiftReflectionContextRef ContextRef,
  void (*Call)(swift_metadata_allocation_t Allocation,
               void *ContextPtr),
  void *ContextPtr) {
  return ContextRef->withContext<const char *>([&](auto *Context) {
    auto Error = Context->iterateMetadataAllocations([&](auto Allocation) {
      swift_metadata_allocation CAllocation;
      CAllocation.Tag = Allocation.Tag;
      CAllocation.Ptr = Allocation.Ptr;
      CAllocation.Size = Allocation.Size;
      Call(CAllocation, ContextPtr);
    });
    return returnableCString(ContextRef, Error);
  });
}

// Convert Allocation to a MetadataAllocation<Runtime>, where <Runtime> is
// the same as the <Runtime> template of Context.
//
// Accepting the Context parameter is a workaround for templated lambda callers
// not having direct access to <Runtime>. The Swift project doesn't compile
// with a new enough C++ version to use explicitly-templated lambdas, so we
// need some other method of extracting <Runtime>.
template <typename Runtime>
static MetadataAllocation<Runtime> convertMetadataAllocation(
    const swift::reflection::ReflectionContext<Runtime> *Context,
    const swift_metadata_allocation_t &Allocation) {
  (void)Context;

  MetadataAllocation<Runtime> ConvertedAllocation;
  ConvertedAllocation.Tag = Allocation.Tag;
  ConvertedAllocation.Ptr = Allocation.Ptr;
  ConvertedAllocation.Size = Allocation.Size;
  return ConvertedAllocation;
}

swift_reflection_ptr_t swift_reflection_allocationMetadataPointer(
  SwiftReflectionContextRef ContextRef,
  swift_metadata_allocation_t Allocation) {
  return ContextRef->withContext<swift_reflection_ptr_t>([&](auto *Context) {
    auto ConvertedAllocation = convertMetadataAllocation(Context, Allocation);
    return Context->allocationMetadataPointer(ConvertedAllocation);
  });
}

const char *swift_reflection_metadataAllocationTagName(
    SwiftReflectionContextRef ContextRef, swift_metadata_allocation_tag_t Tag) {
  return ContextRef->withContext<const char *>([&](auto *Context) {
    auto Result = Context->metadataAllocationTagName(Tag);
    return returnableCString(ContextRef, Result);
  });
}

int swift_reflection_metadataAllocationCacheNode(
    SwiftReflectionContextRef ContextRef,
    swift_metadata_allocation_t Allocation,
    swift_metadata_cache_node_t *OutNode) {
  return ContextRef->withContext<int>([&](auto *Context) {
    auto ConvertedAllocation = convertMetadataAllocation(Context, Allocation);

    auto Result = Context->metadataAllocationCacheNode(ConvertedAllocation);
    if (!Result)
      return 0;

    OutNode->Left = Result->Left;
    OutNode->Right = Result->Right;
    return 1;
  });
}

const char *swift_reflection_iterateMetadataAllocationBacktraces(
    SwiftReflectionContextRef ContextRef,
    swift_metadataAllocationBacktraceIterator Call, void *ContextPtr) {
  return ContextRef->withContext<const char *>([&](auto *Context) {
    auto Error = Context->iterateMetadataAllocationBacktraces(
        [&](auto AllocationPtr, auto Count, auto Ptrs) {
          // Ptrs is an array of StoredPointer, but the callback expects an
          // array of swift_reflection_ptr_t. Those may are not always the same
          // type. (For example, swift_reflection_ptr_t can be 64-bit on 32-bit
          // systems, while StoredPointer is always the pointer size of the
          // target system.) Convert the array to an array of
          // swift_reflection_ptr_t.
          std::vector<swift_reflection_ptr_t> ConvertedPtrs{&Ptrs[0],
                                                            &Ptrs[Count]};
          Call(AllocationPtr, Count, ConvertedPtrs.data(), ContextPtr);
        });
    return returnableCString(ContextRef, Error);
  });
}

swift_async_task_slab_return_t
swift_reflection_asyncTaskSlabPointer(SwiftReflectionContextRef ContextRef,
                                      swift_reflection_ptr_t AsyncTaskPtr) {
  return ContextRef->withContext<swift_async_task_slab_return_t>(
      [&](auto *Context) {
        // We only care about the AllocatorSlabPtr field. Disable child task and
        // async backtrace iteration to save wasted work.
        unsigned ChildTaskLimit = 0;
        unsigned AsyncBacktraceLimit = 0;

        auto [Error, TaskInfo] = Context->asyncTaskInfo(
            AsyncTaskPtr, ChildTaskLimit, AsyncBacktraceLimit);

        swift_async_task_slab_return_t Result = {};
        if (Error) {
          Result.Error = returnableCString(ContextRef, Error);
        }
        Result.SlabPtr = TaskInfo.AllocatorSlabPtr;
        return Result;
      });
}

swift_async_task_slab_allocations_return_t
swift_reflection_asyncTaskSlabAllocations(SwiftReflectionContextRef ContextRef,
                                          swift_reflection_ptr_t SlabPtr) {
  return ContextRef->withContext<swift_async_task_slab_allocations_return_t>(
      [&](auto *Context) {
        auto [Error, Info] = Context->asyncTaskSlabAllocations(SlabPtr);

        swift_async_task_slab_allocations_return_t Result = {};
        if (Result.Error) {
          Result.Error = returnableCString(ContextRef, Error);
          return Result;
        }

        Result.NextSlab = Info.NextSlab;
        Result.SlabSize = Info.SlabSize;

        auto *Chunks = ContextRef->allocateTemporaryObject<
            std::vector<swift_async_task_allocation_chunk_t>>();
        Chunks->reserve(Info.Chunks.size());
        for (auto &Chunk : Info.Chunks) {
          swift_async_task_allocation_chunk_t ConvertedChunk;
          ConvertedChunk.Start = Chunk.Start;
          ConvertedChunk.Length = Chunk.Length;

          // This pedantry is required to properly template over *Context.
          ConvertedChunk.Kind = convertAllocationChunkKind<
              typename std::pointer_traits<decltype(Context)>::element_type>(
              Chunk.Kind);

          Chunks->push_back(ConvertedChunk);
        }

        Result.ChunkCount = Chunks->size();
        Result.Chunks = Chunks->data();

        return Result;
      });
}

swift_async_task_info_t
swift_reflection_asyncTaskInfo(SwiftReflectionContextRef ContextRef,
                               swift_reflection_ptr_t AsyncTaskPtr) {
  return ContextRef->withContext<swift_async_task_info_t>([&](auto *Context) {
    // Limit the child task and async backtrace iteration to semi-reasonable
    // numbers to avoid doing excessive work on bad data.
    unsigned ChildTaskLimit = 1000000;
    unsigned AsyncBacktraceLimit = 1000;

    auto [Error, TaskInfo] = Context->asyncTaskInfo(
        AsyncTaskPtr, ChildTaskLimit, AsyncBacktraceLimit);

    swift_async_task_info_t Result = {};
    if (Error) {
      Result.Error = returnableCString(ContextRef, Error);
      return Result;
    }

    Result.Kind = TaskInfo.Kind;
    Result.EnqueuePriority = TaskInfo.EnqueuePriority;
    Result.IsChildTask = TaskInfo.IsChildTask;
    Result.IsFuture = TaskInfo.IsFuture;
    Result.IsGroupChildTask = TaskInfo.IsGroupChildTask;
    Result.IsAsyncLetTask = TaskInfo.IsAsyncLetTask;

    Result.MaxPriority = TaskInfo.MaxPriority;
    Result.IsCancelled = TaskInfo.IsCancelled;
    Result.IsStatusRecordLocked = TaskInfo.IsStatusRecordLocked;
    Result.IsEscalated = TaskInfo.IsEscalated;
    Result.HasIsRunning = TaskInfo.HasIsRunning;
    Result.IsRunning = TaskInfo.IsRunning;
    Result.IsEnqueued = TaskInfo.IsEnqueued;
    Result.Id = TaskInfo.Id;

    Result.HasThreadPort = TaskInfo.HasThreadPort;
    Result.ThreadPort = TaskInfo.ThreadPort;

    Result.RunJob = TaskInfo.RunJob;
    Result.AllocatorSlabPtr = TaskInfo.AllocatorSlabPtr;

    auto *ChildTasks =
        ContextRef
            ->allocateTemporaryObject<std::vector<swift_reflection_ptr_t>>();
    std::copy(TaskInfo.ChildTasks.begin(), TaskInfo.ChildTasks.end(),
              std::back_inserter(*ChildTasks));
    Result.ChildTaskCount = ChildTasks->size();
    Result.ChildTasks = ChildTasks->data();

    auto *AsyncBacktraceFrames = ContextRef->allocateSubsequentTemporaryObject<
        std::vector<swift_reflection_ptr_t>>();
    std::copy(TaskInfo.AsyncBacktraceFrames.begin(),
              TaskInfo.AsyncBacktraceFrames.end(),
              std::back_inserter(*AsyncBacktraceFrames));
    Result.AsyncBacktraceFramesCount = AsyncBacktraceFrames->size();
    Result.AsyncBacktraceFrames = AsyncBacktraceFrames->data();

    return Result;
  });
}

swift_actor_info_t
swift_reflection_actorInfo(SwiftReflectionContextRef ContextRef,
                           swift_reflection_ptr_t ActorPtr) {
  return ContextRef->withContext<swift_actor_info_t>([&](auto *Context) {
    auto [Error, ActorInfo] = Context->actorInfo(ActorPtr);

    swift_actor_info_t Result = {};
    Result.Error = returnableCString(ContextRef, Error);
    Result.State = ActorInfo.State;
    Result.IsDistributedRemote = ActorInfo.IsDistributedRemote;
    Result.IsPriorityEscalated = ActorInfo.IsPriorityEscalated;
    Result.MaxPriority = ActorInfo.MaxPriority;
    Result.FirstJob = ActorInfo.FirstJob;

    Result.HasThreadPort = ActorInfo.HasThreadPort;
    Result.ThreadPort = ActorInfo.ThreadPort;

    return Result;
  });
}

swift_reflection_ptr_t
swift_reflection_nextJob(SwiftReflectionContextRef ContextRef,
                         swift_reflection_ptr_t JobPtr) {
  return ContextRef->withContext<swift_reflection_ptr_t>([&](auto *Context) {
    return Context->nextJob(JobPtr);
  });
}

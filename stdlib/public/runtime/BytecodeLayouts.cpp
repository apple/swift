//===--- RuntimeValueWitness.cpp - Value Witness Runtime Implementation---===//
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
// Implementations of runtime determined value witness functions
// This file is intended to be statically linked into executables until it is
// fully added to the runtime.
//
//===----------------------------------------------------------------------===//

#include "BytecodeLayouts.h"
#include "../SwiftShims/swift/shims/HeapObject.h"
#include "EnumImpl.h"
#include "WeakReference.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/ABI/System.h"
#include "swift/Runtime/Error.h"
#include "swift/Runtime/HeapObject.h"
#include "llvm/Support/SwapByteOrder.h"
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <type_traits>
#if SWIFT_OBJC_INTEROP
#include "swift/Runtime/ObjCBridge.h"
#include <Block.h>
#endif
#if SWIFT_PTRAUTH
#include <ptrauth.h>
#endif

using namespace swift;

static Metadata *getExistentialTypeMetadata(OpaqueValue *object) {
  return reinterpret_cast<Metadata**>(object)[NumWords_ValueBuffer];
}

template <typename FnTy>
static const FnTy readRelativeFunctionPointer(LayoutStringReader &reader) {
  static_assert(std::is_pointer<FnTy>::value);

  auto absolute = reader.layoutStr + reader.offset;
  auto relativeOffset =
      (uintptr_t)(intptr_t)(int32_t)reader.readBytes<intptr_t>();
  FnTy fn;

#if SWIFT_PTRAUTH
  fn = (FnTy)ptrauth_sign_unauthenticated(
      (void *)((uintptr_t)absolute + relativeOffset),
      ptrauth_key_function_pointer, 0);
#else
  fn = (FnTy)((uintptr_t)absolute + relativeOffset);
#endif

  return fn;
}

typedef Metadata *(*MetadataAccessor)(const Metadata *const *);

static const Metadata *getResilientTypeMetadata(const Metadata *metadata,
                                                LayoutStringReader &reader) {
  auto fn = readRelativeFunctionPointer<MetadataAccessor>(reader);
  return fn(metadata->getGenericArgs());
}

typedef void (*DestrFn)(void*);

struct DestroyFuncAndMask {
  DestrFn fn;
  bool isIndirect;
};

static void skipDestroy(void* ignore) { }

static void existential_destroy(OpaqueValue* object) {
  auto* metadata = getExistentialTypeMetadata(object);
  if (metadata->getValueWitnesses()->isValueInline()) {
    metadata->vw_destroy(object);
  } else {
    swift_release(*(HeapObject**)object);
  }
}

template <typename Handler, typename... Params>
inline static bool handleNextRefCount(const Metadata *metadata,
                                      LayoutStringReader &reader,
                                      uintptr_t &addrOffset, Params... params) {
  uint64_t skip = reader.readBytes<uint64_t>();
  auto tag = static_cast<RefCountingKind>(skip >> 56);
  skip &= ~(0xffULL << 56);
  addrOffset += skip;

  if (SWIFT_UNLIKELY(tag == RefCountingKind::End)) {
    return false;
  } else if (SWIFT_UNLIKELY(tag == RefCountingKind::Metatype)) {
    auto *type = reader.readBytes<const Metadata *>();
    Handler::handleMetatype(type, addrOffset, std::forward<Params>(params)...);
  } else if (SWIFT_UNLIKELY(tag == RefCountingKind::Resilient)) {
    auto *type = getResilientTypeMetadata(metadata, reader);
    Handler::handleMetatype(type, addrOffset, std::forward<Params>(params)...);
  } else if (SWIFT_UNLIKELY(tag ==
                            RefCountingKind::SinglePayloadEnumSimple)) {
    Handler::handleSinglePayloadEnumSimple(reader, addrOffset,
                                           std::forward<Params>(params)...);
  } else if (SWIFT_UNLIKELY(tag == RefCountingKind::SinglePayloadEnumFN)) {
    Handler::handleSinglePayloadEnumFN(reader, false, addrOffset,
                                       std::forward<Params>(params)...);
  } else if (SWIFT_UNLIKELY(tag ==
                            RefCountingKind::SinglePayloadEnumFNResolved)) {
    Handler::handleSinglePayloadEnumFN(reader, true, addrOffset,
                                       std::forward<Params>(params)...);
  } else if (SWIFT_UNLIKELY(tag == RefCountingKind::SinglePayloadEnumGeneric)) {
    Handler::handleSinglePayloadEnumGeneric(reader, addrOffset,
                                            std::forward<Params>(params)...);
  } else if (SWIFT_UNLIKELY(tag == RefCountingKind::MultiPayloadEnumFN)) {
    Handler::handleMultiPayloadEnumFN(metadata, reader, false, addrOffset,
                                      std::forward<Params>(params)...);
  } else if (SWIFT_UNLIKELY(tag ==
                            RefCountingKind::MultiPayloadEnumFNResolved)) {
    Handler::handleMultiPayloadEnumFN(metadata, reader, true, addrOffset,
                                      std::forward<Params>(params)...);
  } else if (SWIFT_UNLIKELY(tag == RefCountingKind::MultiPayloadEnumGeneric)) {
    Handler::handleMultiPayloadEnumGeneric(metadata, reader, addrOffset,
                                           std::forward<Params>(params)...);
  } else {
    Handler::handleReference(tag, addrOffset, std::forward<Params>(params)...);
  }

  return true;
}

template <unsigned N, typename Handler, typename... Params>
inline static void handleRefCounts(const Metadata *metadata,
                                   LayoutStringReader &reader,
                                   uintptr_t &addrOffset, Params... params) {
  if (N == 0) {
    while (handleNextRefCount<Handler>(metadata, reader, addrOffset,
                                       std::forward<Params>(params)...)) {
    }
  } else {
    for (unsigned i = 0; i < N; i++) {
      handleNextRefCount<Handler>(metadata, reader, addrOffset,
                                  std::forward<Params>(params)...);
    }
  }
}

template <unsigned N, typename Handler, typename... Params>
inline static void handleRefCounts(const Metadata *metadata, Params... params) {
  LayoutStringReader reader{metadata->getLayoutString(),
                            layoutStringHeaderSize};
  uintptr_t addrOffset = 0;
  handleRefCounts<N, Handler>(metadata, reader, addrOffset,
                              std::forward<Params>(params)...);
}

static uint64_t readTagBytes(const uint8_t *addr, uint8_t byteCount) {
  switch (byteCount) {
  case 1:
    return addr[0];
  case 2: {
    uint16_t res = 0;
    memcpy(&res, addr, sizeof(uint16_t));
    return res;
  }
  case 4: {
    uint32_t res = 0;
    memcpy(&res, addr, sizeof(uint32_t));
    return res;
  }
  case 8: {
    uint64_t res = 0;
    memcpy(&res, addr, sizeof(uint64_t));
    return res;
  }
  default:
    swift_unreachable("Unsupported tag byte length.");
  }
}

static void handleSinglePayloadEnumSimple(LayoutStringReader &reader,
                                          uint8_t *addr,
                                          uintptr_t &addrOffset) {
  auto byteCountsAndOffset = reader.readBytes<uint64_t>();
  auto extraTagBytesPattern = (uint8_t)(byteCountsAndOffset >> 62);
  auto xiTagBytesPattern = ((uint8_t)(byteCountsAndOffset >> 59)) & 0x7;
  auto xiTagBytesOffset =
      byteCountsAndOffset & std::numeric_limits<uint32_t>::max();

  if (extraTagBytesPattern) {
    auto extraTagBytes = 1 << (extraTagBytesPattern - 1);
    auto payloadSize = reader.readBytes<size_t>();
    auto tagBytes =
        readTagBytes(addr + addrOffset + payloadSize, extraTagBytes);
    if (tagBytes) {
      reader.skip(sizeof(uint64_t) + sizeof(size_t));
      goto noPayload;
    }
  } else {
    reader.skip(sizeof(size_t));
  }

  if (xiTagBytesPattern) {
    auto zeroTagValue = reader.readBytes<uint64_t>();
    auto xiTagValues = reader.readBytes<size_t>();

    auto xiTagBytes = 1 << (xiTagBytesPattern - 1);
    uint64_t tagBytes =
        readTagBytes(addr + addrOffset + xiTagBytesOffset, xiTagBytes) -
        zeroTagValue;
    if (tagBytes >= xiTagValues) {
      reader.skip(sizeof(size_t) * 2);
      return;
    }
  } else {
    reader.skip(sizeof(uint64_t) + sizeof(size_t));
  }

noPayload:
  auto refCountBytes = reader.readBytes<size_t>();
  auto skip = reader.readBytes<size_t>();
  reader.skip(refCountBytes);
  addrOffset += skip;
}

typedef unsigned (*GetEnumTagFn)(const uint8_t *);

static void handleSinglePayloadEnumFN(LayoutStringReader &reader, bool resolved,
                                      uint8_t *addr, uintptr_t &addrOffset) {
  GetEnumTagFn getEnumTag;
  if (resolved) {
    getEnumTag = reader.readBytes<GetEnumTagFn>();
  } else {
    getEnumTag = readRelativeFunctionPointer<GetEnumTagFn>(reader);
  }

  unsigned enumTag = getEnumTag(addr + addrOffset);

  if (enumTag == 0) {
    reader.skip(sizeof(size_t) * 2);
  } else {
    auto refCountBytes = reader.readBytes<size_t>();
    auto skip = reader.readBytes<size_t>();
    reader.skip(refCountBytes);
    addrOffset += skip;
  }
}

static void handleSinglePayloadEnumGeneric(LayoutStringReader &reader,
                                           uint8_t *addr,
                                           uintptr_t &addrOffset) {
  auto tagBytesAndOffset = reader.readBytes<uint64_t>();
  auto extraTagBytesPattern = (uint8_t)(tagBytesAndOffset >> 62);
  auto xiTagBytesOffset =
      tagBytesAndOffset & std::numeric_limits<uint32_t>::max();
  const Metadata *xiType = nullptr;

  if (extraTagBytesPattern) {
    auto extraTagBytes = 1 << (extraTagBytesPattern - 1);
    auto payloadSize = reader.readBytes<size_t>();
    auto tagBytes =
        readTagBytes(addr + addrOffset + payloadSize, extraTagBytes);
    if (tagBytes) {
      reader.skip(sizeof(uint64_t) + sizeof(size_t));
      goto noPayload;
    }
  } else {
    reader.skip(sizeof(size_t));
  }

  xiType = reader.readBytes<const Metadata *>();

  if (xiType) {
    auto numEmptyCases = reader.readBytes<unsigned>();

    auto tag = xiType->vw_getEnumTagSinglePayload(
        (const OpaqueValue *)(addr + addrOffset + xiTagBytesOffset),
        numEmptyCases);
    if (tag == 0) {
      reader.skip(sizeof(size_t) * 2);
      return;
    }
  } else {
    reader.skip(sizeof(uint64_t) + sizeof(size_t));
  }

noPayload:
  auto refCountBytes = reader.readBytes<size_t>();
  auto skip = reader.readBytes<size_t>();
  reader.skip(refCountBytes);
  addrOffset += skip;
}

template <typename Handler, typename... Params>
static void handleMultiPayloadEnumFN(const Metadata *metadata,
                                     LayoutStringReader &reader, bool resolved,
                                     uintptr_t &addrOffset, uint8_t *addr,
                                     Params... params) {
  GetEnumTagFn getEnumTag;
  if (resolved) {
    getEnumTag = reader.readBytes<GetEnumTagFn>();
  } else {
    getEnumTag = readRelativeFunctionPointer<GetEnumTagFn>(reader);
  }

  size_t numPayloads = reader.readBytes<size_t>();
  size_t refCountBytes = reader.readBytes<size_t>();
  size_t enumSize = reader.readBytes<size_t>();

  unsigned enumTag = getEnumTag(addr + addrOffset);

  if (enumTag < numPayloads) {
    size_t refCountOffset = reader.peekBytes<size_t>(enumTag * sizeof(size_t));

    LayoutStringReader nestedReader = reader;
    nestedReader.skip((numPayloads * sizeof(size_t)) + refCountOffset);
    uintptr_t nestedAddrOffset = addrOffset;
    handleRefCounts<0, Handler>(metadata, nestedReader, nestedAddrOffset, addr,
                                std::forward<Params>(params)...);
  }

  reader.skip(refCountBytes + (numPayloads * sizeof(size_t)));
  addrOffset += enumSize;
}

template <typename Handler, typename... Params>
static void handleMultiPayloadEnumGeneric(const Metadata *metadata,
                                          LayoutStringReader &reader,
                                          uintptr_t &addrOffset, uint8_t *addr,
                                          Params... params) {
  auto tagBytes = reader.readBytes<size_t>();
  auto numPayloads = reader.readBytes<size_t>();
  auto refCountBytes = reader.readBytes<size_t>();
  auto enumSize = reader.readBytes<size_t>();
  auto tagBytesOffset = enumSize - tagBytes;

  auto enumTag = readTagBytes(addr + addrOffset + tagBytesOffset, tagBytes);

  if (enumTag < numPayloads) {
    size_t refCountOffset = reader.peekBytes<size_t>(enumTag * sizeof(size_t));

    LayoutStringReader nestedReader = reader;
    nestedReader.skip((numPayloads * sizeof(size_t)) + refCountOffset);
    uintptr_t nestedAddrOffset = addrOffset;
    handleRefCounts<0, Handler>(metadata, nestedReader, nestedAddrOffset, addr,
                                std::forward<Params>(params)...);
  }

  reader.skip(refCountBytes + (numPayloads * sizeof(size_t)));
  addrOffset += enumSize;
}

const DestroyFuncAndMask destroyTable[] = {
  {(DestrFn)&skipDestroy, false},
  {(DestrFn)&swift_errorRelease, true},
  {(DestrFn)&swift_release, true},
  {(DestrFn)&swift_unownedRelease, true},
  {(DestrFn)&swift_weakDestroy, false},
  {(DestrFn)&swift_unknownObjectRelease, true},
  {(DestrFn)&swift_unknownObjectUnownedDestroy, false},
  {(DestrFn)&swift_unknownObjectWeakDestroy, false},
  {(DestrFn)&swift_bridgeObjectRelease, true},
#if SWIFT_OBJC_INTEROP
  {(DestrFn)&_Block_release, true},
  {(DestrFn)&swift_unknownObjectRelease, true},
#else
  {nullptr, true},
  {nullptr, true},
#endif
  // TODO: how to handle Custom?
  {nullptr, true},
  {nullptr, true},
  {nullptr, true},
  {(DestrFn)&existential_destroy, false},
};

struct DestroyHandler {
  static inline void handleMetatype(const Metadata *type, uintptr_t addrOffset,
                                    uint8_t *addr) {
    type->vw_destroy((OpaqueValue *)(addr + addrOffset));
  }

  static inline void handleSinglePayloadEnumSimple(LayoutStringReader &reader,
                                                   uintptr_t &addrOffset,
                                                   uint8_t *addr) {
    ::handleSinglePayloadEnumSimple(reader, addr, addrOffset);
  }

  static inline void handleSinglePayloadEnumFN(LayoutStringReader &reader,
                                               bool resolved,
                                               uintptr_t &addrOffset,
                                               uint8_t *addr) {
    ::handleSinglePayloadEnumFN(reader, resolved, addr, addrOffset);
  }

  static inline void handleSinglePayloadEnumGeneric(LayoutStringReader &reader,
                                                    uintptr_t &addrOffset,
                                                    uint8_t *addr) {
    ::handleSinglePayloadEnumGeneric(reader, addr, addrOffset);
  }

  static inline void handleMultiPayloadEnumFN(const Metadata *metadata,
                                              LayoutStringReader &reader,
                                              bool resolved,
                                              uintptr_t &addrOffset,
                                              uint8_t *addr) {
    ::handleMultiPayloadEnumFN<DestroyHandler>(metadata, reader, resolved,
                                               addrOffset, addr);
  }

  static inline void handleMultiPayloadEnumGeneric(const Metadata *metadata,
                                                   LayoutStringReader &reader,
                                                   uintptr_t &addrOffset,
                                                   uint8_t *addr) {
    ::handleMultiPayloadEnumGeneric<DestroyHandler>(metadata, reader,
                                                    addrOffset, addr);
  }

  static inline void handleReference(RefCountingKind tag, uintptr_t addrOffset,
                                     uint8_t *addr) {
    const auto &destroyFunc = destroyTable[static_cast<uint8_t>(tag)];
    if (SWIFT_LIKELY(destroyFunc.isIndirect)) {
      destroyFunc.fn(
          (void *)((*(uintptr_t *)(addr + addrOffset))));
    } else {
      destroyFunc.fn(((void *)(addr + addrOffset)));
    }
  }
};

extern "C" void
swift_generic_destroy(swift::OpaqueValue *address, const Metadata *metadata) {
  handleRefCounts<0, DestroyHandler>(metadata, (uint8_t *)address);
}

struct RetainFuncAndMask {
  void* fn;
  bool isSingle;
};

#if SWIFT_OBJC_INTEROP
void* Block_copyForwarder(void** dest, const void** src) {
  *dest = _Block_copy(*src);
  return *dest;
}
#endif

typedef void* (*RetainFn)(void*);
typedef void* (*CopyInitFn)(void*, void*);

void* skipRetain(void* ignore) { return nullptr; }
void* existential_initializeWithCopy(OpaqueValue* dest, OpaqueValue* src) {
  auto* metadata = getExistentialTypeMetadata(src);
  return metadata->vw_initializeBufferWithCopyOfBuffer((ValueBuffer*)dest,
                                                       (ValueBuffer*)src);
}

const RetainFuncAndMask retainTable[] = {
  {(void*)&skipRetain, true},
  {(void*)&swift_errorRetain, true},
  {(void*)&swift_retain, true},
  {(void*)&swift_unownedRetain, true},
  {(void*)&swift_weakCopyInit, false},
  {(void*)&swift_unknownObjectRetain, true},
  {(void*)&swift_unknownObjectUnownedCopyInit, false},
  {(void*)&swift_unknownObjectWeakCopyInit, false},
  {(void*)&swift_bridgeObjectRetain, true},
#if SWIFT_OBJC_INTEROP
  {(void*)&Block_copyForwarder, false},
  {(void*)&objc_retain, true},
#else
  {nullptr, true},
  {nullptr, true},
#endif
  // TODO: how to handle Custom?
  {nullptr, true},
  {nullptr, true},
  {nullptr, true},
  {(void*)&existential_initializeWithCopy, false},
};

struct CopyHandler {
  static inline void handleMetatype(const Metadata *type, uintptr_t addrOffset,
                                    uint8_t *dest, uint8_t *src) {
    type->vw_initializeWithCopy((OpaqueValue*)((uintptr_t)dest + addrOffset),
                                (OpaqueValue*)((uintptr_t)src + addrOffset));
  }

  static inline void handleSinglePayloadEnumSimple(LayoutStringReader &reader,
                                                   uintptr_t &addrOffset,
                                                   uint8_t *dest,
                                                   uint8_t *src) {
    ::handleSinglePayloadEnumSimple(reader, src, addrOffset);
  }

  static inline void handleSinglePayloadEnumFN(LayoutStringReader &reader,
                                               bool resolved,
                                               uintptr_t &addrOffset,
                                               uint8_t *dest, uint8_t *src) {
    ::handleSinglePayloadEnumFN(reader, resolved, src, addrOffset);
  }

  static inline void handleSinglePayloadEnumGeneric(LayoutStringReader &reader,
                                                    uintptr_t &addrOffset,
                                                    uint8_t *dest,
                                                    uint8_t *src) {
    ::handleSinglePayloadEnumGeneric(reader, src, addrOffset);
  }

  static inline void handleMultiPayloadEnumFN(const Metadata *metadata,
                                              LayoutStringReader &reader,
                                              bool resolved,
                                              uintptr_t &addrOffset,
                                              uint8_t *dest, uint8_t *src) {
    ::handleMultiPayloadEnumFN<CopyHandler>(metadata, reader, resolved,
                                            addrOffset, dest, src);
  }

  static inline void handleMultiPayloadEnumGeneric(const Metadata *metadata,
                                                   LayoutStringReader &reader,
                                                   uintptr_t &addrOffset,
                                                   uint8_t *dest,
                                                   uint8_t *src) {
    ::handleMultiPayloadEnumGeneric<CopyHandler>(metadata, reader, addrOffset,
                                                 dest, src);
  }

  static inline void handleReference(RefCountingKind tag, uintptr_t addrOffset,
                                     uint8_t *dest, uint8_t *src) {
    const auto &retainFunc = retainTable[static_cast<uint8_t>(tag)];
    if (SWIFT_LIKELY(retainFunc.isSingle)) {
      ((RetainFn)retainFunc.fn)(*(void**)(((uintptr_t)dest + addrOffset)));
    } else {
      ((CopyInitFn)retainFunc.fn)((void*)((uintptr_t)dest + addrOffset),
                                  (void*)((uintptr_t)src + addrOffset));
    }
  }
};

extern "C" swift::OpaqueValue *
swift_generic_initWithCopy(swift::OpaqueValue *dest, swift::OpaqueValue *src,
                           const Metadata *metadata) {
  size_t size = metadata->vw_size();
  memcpy(dest, src, size);

  handleRefCounts<0, CopyHandler>(metadata, (uint8_t *)dest, (uint8_t *)src);

  return dest;
}

struct TakeHandler {
  static inline void handleMetatype(const Metadata *type, uintptr_t addrOffset,
                                    uint8_t *dest, uint8_t *src) {
    if (SWIFT_UNLIKELY(!type->getValueWitnesses()->isBitwiseTakable())) {
      type->vw_initializeWithTake(
          (OpaqueValue*)((uintptr_t)dest + addrOffset),
          (OpaqueValue*)((uintptr_t)src + addrOffset));
    }
  }

  static inline void handleSinglePayloadEnumSimple(LayoutStringReader &reader,
                                                   uintptr_t &addrOffset,
                                                   uint8_t *dest,
                                                   uint8_t *src) {
    ::handleSinglePayloadEnumSimple(reader, src, addrOffset);
  }

  static inline void handleSinglePayloadEnumFN(LayoutStringReader &reader,
                                               bool resolved,
                                               uintptr_t &addrOffset,
                                               uint8_t *dest, uint8_t *src) {
    ::handleSinglePayloadEnumFN(reader, resolved, src, addrOffset);
  }

  static inline void handleSinglePayloadEnumGeneric(LayoutStringReader &reader,
                                                    uintptr_t &addrOffset,
                                                    uint8_t *dest,
                                                    uint8_t *src) {
    ::handleSinglePayloadEnumGeneric(reader, src, addrOffset);
  }

  static inline void handleMultiPayloadEnumFN(const Metadata *metadata,
                                              LayoutStringReader &reader,
                                              bool resolved,
                                              uintptr_t &addrOffset,
                                              uint8_t *dest, uint8_t *src) {
    ::handleMultiPayloadEnumFN<TakeHandler>(metadata, reader, resolved,
                                            addrOffset, dest, src);
  }

  static inline void handleMultiPayloadEnumGeneric(const Metadata *metadata,
                                                   LayoutStringReader &reader,
                                                   uintptr_t &addrOffset,
                                                   uint8_t *dest,
                                                   uint8_t *src) {
    ::handleMultiPayloadEnumGeneric<TakeHandler>(metadata, reader, addrOffset,
                                                 dest, src);
  }

  static inline void handleReference(RefCountingKind tag, uintptr_t addrOffset,
                                     uint8_t *dest, uint8_t *src) {
    if (tag == RefCountingKind::UnknownWeak) {
      swift_unknownObjectWeakTakeInit(
        (WeakReference*)((uintptr_t)dest + addrOffset),
        (WeakReference*)((uintptr_t)src + addrOffset));
    } else if (tag == RefCountingKind::Existential) {
      auto *type = getExistentialTypeMetadata(
          (OpaqueValue*)((uintptr_t)src + addrOffset));
      if (SWIFT_UNLIKELY(!type->getValueWitnesses()->isBitwiseTakable())) {
        type->vw_initializeWithTake(
            (OpaqueValue *)((uintptr_t)dest + addrOffset),
            (OpaqueValue *)((uintptr_t)src + addrOffset));
      }
    }
  }
};

extern "C" swift::OpaqueValue *
swift_generic_initWithTake(swift::OpaqueValue *dest, swift::OpaqueValue *src,
                           const Metadata *metadata) {
  size_t size = metadata->vw_size();

  memcpy(dest, src, size);

  if (SWIFT_LIKELY(metadata->getValueWitnesses()->isBitwiseTakable())) {
    return dest;
  }

  handleRefCounts<0, TakeHandler>(metadata, (uint8_t *)dest, (uint8_t *)src);

  return dest;
}

extern "C" swift::OpaqueValue *
swift_generic_assignWithCopy(swift::OpaqueValue *dest, swift::OpaqueValue *src,
                             const Metadata *metadata) {
  swift_generic_destroy(dest, metadata);
  return swift_generic_initWithCopy(dest, src, metadata);
}

extern "C" swift::OpaqueValue *
swift_generic_assignWithTake(swift::OpaqueValue *dest, swift::OpaqueValue *src,
                             const Metadata *metadata) {
  swift_generic_destroy(dest, metadata);
  return swift_generic_initWithTake(dest, src, metadata);
}

extern "C" unsigned swift_singletonEnum_getEnumTag(swift::OpaqueValue *address,
                                                   const Metadata *metadata) {
  return 0;
}

extern "C" void swift_singletonEnum_destructiveInjectEnumTag(
    swift::OpaqueValue *address, unsigned tag, const Metadata *metadata) {
  return;
}

template <typename T>
static inline T handleSinglePayloadEnumSimpleTag(
    LayoutStringReader &reader, uint8_t *addr,
    std::function<std::optional<T>(size_t, size_t, uint8_t)>
        extraTagBytesHandler,
    std::function<T(size_t, uint64_t, uint8_t, unsigned, size_t, uint8_t)>
        xiHandler) {
  auto byteCountsAndOffset = reader.readBytes<uint64_t>();
  auto extraTagBytesPattern = (uint8_t)(byteCountsAndOffset >> 62);
  auto xiTagBytesPattern = ((uint8_t)(byteCountsAndOffset >> 59)) & 0x7;
  auto xiTagBytesOffset =
      byteCountsAndOffset & std::numeric_limits<uint32_t>::max();
  auto numExtraTagBytes = 1 << (extraTagBytesPattern - 1);
  auto payloadSize = reader.readBytes<size_t>();
  auto zeroTagValue = reader.readBytes<uint64_t>();
  auto payloadNumExtraInhabitants = reader.readBytes<size_t>();

  if (extraTagBytesPattern) {
    if (auto result = extraTagBytesHandler(payloadNumExtraInhabitants,
                                           payloadSize, numExtraTagBytes)) {
      return *result;
    }
  }

  return xiHandler(payloadNumExtraInhabitants, zeroTagValue, xiTagBytesPattern,
                   xiTagBytesOffset, payloadSize, numExtraTagBytes);
}

extern "C" unsigned swift_enumSimple_getEnumTag(swift::OpaqueValue *address,
                                                const Metadata *metadata) {
  auto addr = reinterpret_cast<uint8_t *>(address);
  LayoutStringReader reader{metadata->getLayoutString(),
                            layoutStringHeaderSize + sizeof(uint64_t)};

  auto extraTagBytesHandler =
      [addr](size_t payloadNumExtraInhabitants, size_t payloadSize,
             uint8_t numExtraTagBytes) -> std::optional<unsigned> {
    auto tagBytes = readTagBytes(addr + payloadSize, numExtraTagBytes);
    if (tagBytes) {
      unsigned caseIndexFromExtraTagBits =
          payloadSize >= 4 ? 0 : (tagBytes - 1U) << (payloadSize * 8U);
      unsigned caseIndexFromValue = loadEnumElement(addr, payloadSize);
      unsigned noPayloadIndex =
          (caseIndexFromExtraTagBits | caseIndexFromValue) +
          payloadNumExtraInhabitants;
      return noPayloadIndex + 1;
    }

    return std::nullopt;
  };

  auto xihandler = [addr](size_t payloadNumExtraInhabitants,
                          uint64_t zeroTagValue, uint8_t xiTagBytesPattern,
                          unsigned xiTagBytesOffset, size_t payloadSize,
                          uint8_t numExtraTagBytes) -> unsigned {
    auto xiTagBytes = 1 << (xiTagBytesPattern - 1);
    uint64_t tagBytes =
        readTagBytes(addr + xiTagBytesOffset, xiTagBytes) -
        zeroTagValue;
    if (tagBytes < payloadNumExtraInhabitants) {
      return tagBytes + 1;
    }

    return 0;
  };

  return handleSinglePayloadEnumSimpleTag<unsigned>(
      reader, addr, extraTagBytesHandler, xihandler);
}

extern "C" void swift_enumSimple_destructiveInjectEnumTag(
    swift::OpaqueValue *address, unsigned tag, const Metadata *metadata) {
  auto addr = reinterpret_cast<uint8_t *>(address);
  LayoutStringReader reader{metadata->getLayoutString(),
                            layoutStringHeaderSize + sizeof(uint64_t)};

  auto extraTagBytesHandler =
      [addr, tag](size_t payloadNumExtraInhabitants, size_t payloadSize,
                  uint8_t numExtraTagBytes) -> std::optional<bool> {
    if (tag <= payloadNumExtraInhabitants) {
      return std::nullopt;
    }

    unsigned noPayloadIndex = tag - 1;
    unsigned caseIndex = noPayloadIndex - payloadNumExtraInhabitants;
    unsigned payloadIndex, extraTagIndex;
    if (payloadSize >= 4) {
      extraTagIndex = 1;
      payloadIndex = caseIndex;
    } else {
      unsigned payloadBits = payloadSize * 8U;
      extraTagIndex = 1U + (caseIndex >> payloadBits);
      payloadIndex = caseIndex & ((1U << payloadBits) - 1U);
    }

    // Store into the value.
    if (payloadSize)
      storeEnumElement(addr, payloadIndex, payloadSize);
    if (numExtraTagBytes)
      storeEnumElement(addr + payloadSize, extraTagIndex, numExtraTagBytes);

    return true;
  };

  auto xihandler = [addr, tag](size_t payloadNumExtraInhabitants,
                               uint64_t zeroTagValue, uint8_t xiTagBytesPattern,
                               unsigned xiTagBytesOffset, size_t payloadSize,
                               uint8_t numExtraTagBytes) -> bool {
    auto xiTagBytes = 1 << (xiTagBytesPattern - 1);
    if (tag <= payloadNumExtraInhabitants) {
      if (numExtraTagBytes != 0)
        storeEnumElement(addr + payloadSize, 0, numExtraTagBytes);

      if (tag == 0)
        return true;

      storeEnumElement(addr + xiTagBytesOffset, tag - 1 + zeroTagValue,
                       xiTagBytes);
    }
    return true;
  };

  handleSinglePayloadEnumSimpleTag<bool>(reader, addr, extraTagBytesHandler,
                                         xihandler);
}

extern "C"
unsigned swift_enumFn_getEnumTag(swift::OpaqueValue *address,
                                 const Metadata *metadata) {
  auto addr = reinterpret_cast<const uint8_t *>(address);
  LayoutStringReader reader{metadata->getLayoutString(),
                            layoutStringHeaderSize + sizeof(uint64_t)};
  auto getEnumTag = readRelativeFunctionPointer<GetEnumTagFn>(reader);

  return getEnumTag(addr);
}

extern "C" unsigned
swift_multiPayloadEnumGeneric_getEnumTag(swift::OpaqueValue *address,
                                         const Metadata *metadata) {
  auto addr = reinterpret_cast<const uint8_t *>(address);
  LayoutStringReader reader{metadata->getLayoutString(),
                            layoutStringHeaderSize + sizeof(uint64_t)};

  auto tagBytes = reader.readBytes<size_t>();
  auto numPayloads = reader.readBytes<size_t>();
  reader.skip(sizeof(size_t));
  auto enumSize = reader.readBytes<size_t>();
  auto payloadSize = enumSize - tagBytes;

  auto enumTag = (unsigned)readTagBytes(addr + payloadSize, tagBytes);
  if (enumTag < numPayloads) {
    return enumTag;
  }

  auto payloadValue = loadEnumElement(addr, payloadSize);

  if (payloadSize >= 4) {
    return numPayloads + payloadValue;
  } else {
    unsigned numPayloadBits = payloadSize * CHAR_BIT;
    return (payloadValue | (enumTag - numPayloads) << numPayloadBits) +
           numPayloads;
  }
}

extern "C" void swift_multiPayloadEnumGeneric_destructiveInjectEnumTag(
    swift::OpaqueValue *address, unsigned tag, const Metadata *metadata) {
  auto addr = reinterpret_cast<uint8_t *>(address);
  LayoutStringReader reader{metadata->getLayoutString(),
                            layoutStringHeaderSize + sizeof(uint64_t)};

  auto numTagBytes = reader.readBytes<size_t>();
  auto numPayloads = reader.readBytes<size_t>();
  reader.skip(sizeof(size_t));
  auto enumSize = reader.readBytes<size_t>();
  auto payloadSize = enumSize - numTagBytes;

  if (tag < numPayloads) {
    // For a payload case, store the tag after the payload area.
    auto tagBytes = addr + payloadSize;
    storeEnumElement(tagBytes, tag, numTagBytes);
  } else {
    // For an empty case, factor out the parts that go in the payload and
    // tag areas.
    unsigned whichEmptyCase = tag - numPayloads;
    unsigned whichTag, whichPayloadValue;
    if (payloadSize >= 4) {
      whichTag = numPayloads;
      whichPayloadValue = whichEmptyCase;
    } else {
      unsigned numPayloadBits = payloadSize * CHAR_BIT;
      whichTag = numPayloads + (whichEmptyCase >> numPayloadBits);
      whichPayloadValue = whichEmptyCase & ((1U << numPayloadBits) - 1U);
    }
    auto tagBytes = addr + payloadSize;
    storeEnumElement(tagBytes, whichTag, numTagBytes);
    storeEnumElement(addr, whichPayloadValue, payloadSize);
  }
}

template <typename T>
static inline T handleSinglePayloadEnumGenericTag(
    LayoutStringReader &reader, uint8_t *addr,
    std::function<std::optional<T>(const Metadata *, size_t, uint8_t)>
        extraTagBytesHandler,
    std::function<T(const Metadata *, unsigned, unsigned, size_t, uint8_t)>
        xiHandler) {
  auto tagBytesAndOffset = reader.readBytes<uint64_t>();
  auto extraTagBytesPattern = (uint8_t)(tagBytesAndOffset >> 62);
  auto xiTagBytesOffset =
      tagBytesAndOffset & std::numeric_limits<uint32_t>::max();
  auto numExtraTagBytes = 1 << (extraTagBytesPattern - 1);
  auto payloadSize = reader.readBytes<size_t>();
  auto xiType = reader.readBytes<const Metadata *>();

  if (extraTagBytesPattern) {
    if (auto result =
            extraTagBytesHandler(xiType, payloadSize, numExtraTagBytes)) {
      return *result;
    }
  }

  auto numEmptyCases = reader.readBytes<unsigned>();

  return xiHandler(xiType, xiTagBytesOffset, numEmptyCases, payloadSize,
                   numExtraTagBytes);
}

extern "C" unsigned
swift_singlePayloadEnumGeneric_getEnumTag(swift::OpaqueValue *address,
                                          const Metadata *metadata) {
  auto addr = reinterpret_cast<uint8_t *>(address);
  LayoutStringReader reader{metadata->getLayoutString(),
                            layoutStringHeaderSize + sizeof(uint64_t)};

  auto extraTagBytesHandler =
      [addr](const Metadata *xiType, size_t payloadSize,
             uint8_t numExtraTagBytes) -> std::optional<unsigned> {
    auto tagBytes = readTagBytes(addr + payloadSize, numExtraTagBytes);
    if (tagBytes) {
      unsigned payloadNumExtraInhabitants =
          xiType ? xiType->vw_getNumExtraInhabitants() : 0;
      unsigned caseIndexFromExtraTagBits =
          payloadSize >= 4 ? 0 : (tagBytes - 1U) << (payloadSize * 8U);
      unsigned caseIndexFromValue = loadEnumElement(addr, payloadSize);
      unsigned noPayloadIndex =
          (caseIndexFromExtraTagBits | caseIndexFromValue) +
          payloadNumExtraInhabitants;
      return noPayloadIndex + 1;
    }

    return std::nullopt;
  };

  auto xihandler = [addr](const Metadata *xiType, unsigned xiTagBytesOffset,
                          unsigned numEmptyCases, size_t payloadSize,
                          uint8_t numExtraTagBytes) -> unsigned {
    if (xiType) {
      return xiType->vw_getEnumTagSinglePayload(
          (const OpaqueValue *)(addr + xiTagBytesOffset), numEmptyCases);
    }

    return 0;
  };

  return handleSinglePayloadEnumGenericTag<unsigned>(
      reader, addr, extraTagBytesHandler, xihandler);
}

extern "C" void swift_singlePayloadEnumGeneric_destructiveInjectEnumTag(
    swift::OpaqueValue *address, unsigned tag, const Metadata *metadata) {
  auto addr = reinterpret_cast<uint8_t *>(address);
  LayoutStringReader reader{metadata->getLayoutString(),
                            layoutStringHeaderSize + sizeof(uint64_t)};

  auto extraTagBytesHandler =
      [=](const Metadata *xiType, size_t payloadSize,
          uint8_t numExtraTagBytes) -> std::optional<bool> {
    unsigned payloadNumExtraInhabitants =
        xiType ? xiType->vw_getNumExtraInhabitants() : 0;
    if (tag <= payloadNumExtraInhabitants) {
      return std::nullopt;
    }

    unsigned noPayloadIndex = tag - 1;
    unsigned caseIndex = noPayloadIndex - payloadNumExtraInhabitants;
    unsigned payloadIndex, extraTagIndex;
    if (payloadSize >= 4) {
      extraTagIndex = 1;
      payloadIndex = caseIndex;
    } else {
      unsigned payloadBits = payloadSize * 8U;
      extraTagIndex = 1U + (caseIndex >> payloadBits);
      payloadIndex = caseIndex & ((1U << payloadBits) - 1U);
    }

    // Store into the value.
    if (payloadSize)
      storeEnumElement(addr, payloadIndex, payloadSize);
    if (numExtraTagBytes)
      storeEnumElement(addr + payloadSize, extraTagIndex, numExtraTagBytes);

    return true;
  };

  auto xihandler = [=](const Metadata *xiType, unsigned xiTagBytesOffset,
                       unsigned numEmptyCases, size_t payloadSize,
                       uint8_t numExtraTagBytes) -> bool {
    unsigned payloadNumExtraInhabitants =
        xiType ? xiType->vw_getNumExtraInhabitants() : 0;
    if (tag <= payloadNumExtraInhabitants) {
      if (numExtraTagBytes != 0)
        storeEnumElement(addr + payloadSize, 0, numExtraTagBytes);

      if (tag == 0)
        return true;

      xiType->vw_storeEnumTagSinglePayload(
          (swift::OpaqueValue *)(addr + xiTagBytesOffset), tag, numEmptyCases);
    }
    return true;
  };

  handleSinglePayloadEnumGenericTag<bool>(reader, addr, extraTagBytesHandler,
                                          xihandler);
}

extern "C" swift::OpaqueValue *
swift_generic_initializeBufferWithCopyOfBuffer(swift::ValueBuffer *dest,
                                               swift::ValueBuffer *src,
                                               const Metadata *metadata) {
  if (metadata->getValueWitnesses()->isValueInline()) {
    return swift_generic_initWithCopy((swift::OpaqueValue *)dest,
                                      (swift::OpaqueValue *)src, metadata);
  } else {
    memcpy(dest, src, sizeof(swift::HeapObject *));
    swift_retain(*(swift::HeapObject **)src);
    return (swift::OpaqueValue *)&(*(swift::HeapObject **)dest)[1];
  }
}

void swift::swift_resolve_resilientAccessors(uint8_t *layoutStr,
                                             size_t layoutStrOffset,
                                             const uint8_t *fieldLayoutStr,
                                             const Metadata *fieldType) {
  LayoutStringWriter writer{layoutStr, layoutStrOffset};
  LayoutStringReader reader{fieldLayoutStr, layoutStringHeaderSize};
  while (true) {
    size_t currentOffset = reader.offset;
    uint64_t size = reader.readBytes<uint64_t>();
    RefCountingKind tag = (RefCountingKind)(size >> 56);
    size &= ~(0xffULL << 56);

    switch (tag) {
    case RefCountingKind::End:
      return;
    case RefCountingKind::Resilient: {
      auto *type = getResilientTypeMetadata(fieldType, reader);
      writer.offset = layoutStrOffset + currentOffset - layoutStringHeaderSize;
      uint64_t tagAndOffset =
          (((uint64_t)RefCountingKind::Metatype) << 56) | size;
      writer.writeBytes(tagAndOffset);
      writer.writeBytes(type);
      break;
    }
    case RefCountingKind::Metatype:
      reader.skip(sizeof(uintptr_t));
      break;
    case RefCountingKind::SinglePayloadEnumSimple:
      reader.skip((3 * sizeof(uint64_t)) + (4 * sizeof(size_t)));
      break;

    case RefCountingKind::SinglePayloadEnumFN: {
      auto getEnumTag = readRelativeFunctionPointer<GetEnumTagFn>(reader);
      writer.offset = layoutStrOffset + currentOffset - layoutStringHeaderSize;
      uint64_t tagAndOffset =
          (((uint64_t)RefCountingKind::SinglePayloadEnumFNResolved) << 56) |
          size;
      writer.writeBytes(tagAndOffset);
      writer.writeBytes(getEnumTag);
      reader.skip(2 * sizeof(size_t));
      break;
    }

    case RefCountingKind::SinglePayloadEnumFNResolved:
      reader.skip(3 * sizeof(size_t));
      break;

    case RefCountingKind::SinglePayloadEnumGeneric: {
      reader.skip(sizeof(uint64_t) +  // tag + offset
                  sizeof(uint64_t) +  // extra tag bytes + XI offset
                  sizeof(size_t) +    // payload size
                  sizeof(uintptr_t) + // XI metadata
                  sizeof(unsigned));  // num empty cases
      auto refCountBytes = reader.readBytes<size_t>();
      reader.skip(sizeof(size_t) + // bytes to skip if no payload case
                  refCountBytes);
      break;
    }

    case RefCountingKind::MultiPayloadEnumFN: {
      auto getEnumTag = readRelativeFunctionPointer<GetEnumTagFn>(reader);
      writer.offset = layoutStrOffset + currentOffset - layoutStringHeaderSize;
      uint64_t tagAndOffset =
          (((uint64_t)RefCountingKind::MultiPayloadEnumFNResolved) << 56) |
          size;
      writer.writeBytes(tagAndOffset);
      writer.writeBytes(getEnumTag);

      size_t numCases = reader.readBytes<size_t>();
      // skip ref count bytes
      reader.skip(sizeof(size_t));

      size_t casesBeginOffset =
          layoutStrOffset + reader.offset + (numCases * sizeof(size_t));

      for (size_t j = 0; j < numCases; j++) {
        size_t caseOffset = reader.readBytes<size_t>();
        const uint8_t *caseLayoutString = fieldLayoutStr + reader.offset +
                                          (numCases * sizeof(size_t)) +
                                          caseOffset;
        swift_resolve_resilientAccessors(layoutStr,
                                         casesBeginOffset + caseOffset,
                                         caseLayoutString, fieldType);
      }
      break;
    }

    case RefCountingKind::MultiPayloadEnumFNResolved: {
      // skip function pointer
      reader.skip(sizeof(uintptr_t));
      size_t numCases = reader.readBytes<size_t>();
      size_t refCountBytes = reader.readBytes<size_t>();
      // skip enum size, offsets and ref counts
      reader.skip(sizeof(size_t) + (numCases * sizeof(size_t)) + refCountBytes);
      break;
    }

    case RefCountingKind::MultiPayloadEnumGeneric: {
      reader.skip(sizeof(size_t));
      auto numPayloads = reader.readBytes<size_t>();
      auto refCountBytes = reader.readBytes<size_t>();
      reader.skip(sizeof(size_t) * (numPayloads + 1) + refCountBytes);
      break;
    }

    default:
      break;
    }
  }
}

extern "C"
void swift_generic_instantiateLayoutString(const uint8_t* layoutStr,
                                           Metadata* type) {
  type->setLayoutString(layoutStr);
}

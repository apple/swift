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
#include "WeakReference.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/ABI/System.h"
#include "swift/Runtime/Error.h"
#include "swift/Runtime/HeapObject.h"
#include "llvm/Support/SwapByteOrder.h"
#include <cstdint>
#include <limits>
#include <type_traits>
#if SWIFT_OBJC_INTEROP
#include "swift/Runtime/ObjCBridge.h"
#include <Block.h>
#endif
#if SWIFT_PTRAUTH
#include <ptrauth.h>
#endif

using namespace swift;

static const size_t layoutStringHeaderSize = sizeof(uint64_t) + sizeof(size_t);

/// Given a pointer and an offset, read the requested data and increment the
/// offset
template <typename T>
static T readBytes(const uint8_t *typeLayout, size_t &i) {
  T returnVal;
  memcpy(&returnVal, typeLayout + i, sizeof(T));
  i += sizeof(T);
  return returnVal;
}

/// Given a pointer, a value, and an offset, write the value at the given
/// offset and increment offset by the size of T
template <typename T>
static void writeBytes(uint8_t *typeLayout, size_t &i, T value) {
  memcpy(typeLayout + i, &value, sizeof(T));
  i += sizeof(T);
}

static Metadata *getExistentialTypeMetadata(OpaqueValue *object) {
  return reinterpret_cast<Metadata**>(object)[NumWords_ValueBuffer];
}

template <typename FnTy>
static const FnTy readRelativeFunctionPointer(const uint8_t *layoutStr,
                                              size_t &offset) {
  static_assert(std::is_pointer<FnTy>::value);

  auto absolute = layoutStr + offset;
  auto relativeOffset =
      (uintptr_t)(intptr_t)(int32_t)readBytes<intptr_t>(layoutStr, offset);
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
                                                const uint8_t *layoutStr,
                                                size_t &offset) {
  auto fn = readRelativeFunctionPointer<MetadataAccessor>(layoutStr, offset);
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
                                      const uint8_t *typeLayout, size_t &offset,
                                      uintptr_t &addrOffset, Params... params) {
  uint64_t skip = readBytes<uint64_t>(typeLayout, offset);
  auto tag = static_cast<RefCountingKind>(skip >> 56);
  skip &= ~(0xffULL << 56);
  addrOffset += skip;

  if (SWIFT_UNLIKELY(tag == RefCountingKind::End)) {
    return false;
  } else if (SWIFT_UNLIKELY(tag == RefCountingKind::Metatype)) {
    auto *type = readBytes<const Metadata*>(typeLayout, offset);
    Handler::handleMetatype(type, addrOffset, std::forward<Params>(params)...);
  } else if (SWIFT_UNLIKELY(tag == RefCountingKind::Resilient)) {
    auto *type = getResilientTypeMetadata(metadata, typeLayout, offset);
    Handler::handleMetatype(type, addrOffset, std::forward<Params>(params)...);
  } else if (SWIFT_UNLIKELY(tag ==
                            RefCountingKind::SinglePayloadEnumSimple)) {
    Handler::handleSinglePayloadEnumSimple(typeLayout, offset, addrOffset,
                                           std::forward<Params>(params)...);
  } else if (SWIFT_UNLIKELY(tag == RefCountingKind::SinglePayloadEnumFN)) {
    Handler::handleSinglePayloadEnumFN(typeLayout, offset, false, addrOffset,
                                       std::forward<Params>(params)...);
  } else if (SWIFT_UNLIKELY(tag ==
                            RefCountingKind::SinglePayloadEnumFNResolved)) {
    Handler::handleSinglePayloadEnumFN(typeLayout, offset, true, addrOffset,
                                       std::forward<Params>(params)...);
  } else if (SWIFT_UNLIKELY(tag == RefCountingKind::MultiPayloadEnumFN)) {
    Handler::handleMultiPayloadEnumFN(metadata, typeLayout, offset, false,
                                      addrOffset,
                                      std::forward<Params>(params)...);
  } else if (SWIFT_UNLIKELY(tag ==
                            RefCountingKind::MultiPayloadEnumFNResolved)) {
    Handler::handleMultiPayloadEnumFN(metadata, typeLayout, offset, true,
                                      addrOffset,
                                      std::forward<Params>(params)...);
  } else {
    Handler::handleReference(tag, addrOffset, std::forward<Params>(params)...);
  }

  return true;
}

template <unsigned N, typename Handler, typename... Params>
inline static void handleRefCounts(const Metadata *metadata,
                                   const uint8_t *typeLayout, size_t &offset,
                                   uintptr_t &addrOffset, Params... params) {
  if (N == 0) {
    while (handleNextRefCount<Handler>(metadata, typeLayout, offset, addrOffset,
                                       std::forward<Params>(params)...)) {
    }
  } else {
    for (int i = 0; i < N; i++) {
      handleNextRefCount<Handler>(metadata, typeLayout, offset, addrOffset,
                                  std::forward<Params>(params)...);
    }
  }
}

template <unsigned N, typename Handler, typename... Params>
inline static void handleRefCounts(const Metadata *metadata, Params... params) {
  const uint8_t *typeLayout = metadata->getLayoutString();
  size_t offset = layoutStringHeaderSize;
  uintptr_t addrOffset = 0;
  handleRefCounts<N, Handler>(metadata, typeLayout, offset, addrOffset,
                              std::forward<Params>(params)...);
}

static uint64_t readTagBytes(uint8_t *addr, uint8_t byteCount) {
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

static void handleSinglePayloadEnumSimple(const uint8_t *typeLayout,
                                          size_t &offset, uint8_t *addr,
                                          uintptr_t &addrOffset) {
  auto byteCountsAndOffset = readBytes<uint64_t>(typeLayout, offset);
  auto extraTagBytesPattern = (uint8_t)(byteCountsAndOffset >> 62);
  auto xiTagBytesPattern = ((uint8_t)(byteCountsAndOffset >> 59)) & 0x7;
  auto xiTagBytesOffset =
      byteCountsAndOffset & std::numeric_limits<uint32_t>::max();

  if (extraTagBytesPattern) {
    auto extraTagBytes = 1 << (extraTagBytesPattern - 1);
    auto payloadSize = readBytes<size_t>(typeLayout, offset);
    auto tagBytes =
        readTagBytes(addr + addrOffset + payloadSize, extraTagBytes);
    if (tagBytes) {
      offset += sizeof(uint64_t) + sizeof(size_t);
      goto noPayload;
    }
  } else {
    offset += sizeof(size_t);
  }

  if (xiTagBytesPattern) {
    auto zeroTagValue = readBytes<uint64_t>(typeLayout, offset);
    auto xiTagValues = readBytes<size_t>(typeLayout, offset);

    auto xiTagBytes = 1 << (xiTagBytesPattern - 1);
    uint64_t tagBytes =
        readTagBytes(addr + addrOffset + xiTagBytesOffset, xiTagBytes) -
        zeroTagValue;
    if (tagBytes >= xiTagValues) {
      offset += sizeof(size_t) * 2;
      return;
    }
  } else {
    offset += sizeof(uint64_t) + sizeof(size_t);
  }

noPayload:
  auto refCountBytes = readBytes<size_t>(typeLayout, offset);
  auto skip = readBytes<size_t>(typeLayout, offset);
  offset += refCountBytes;
  addrOffset += skip;
}

typedef unsigned (*GetEnumTagFn)(const uint8_t *);

static void handleSinglePayloadEnumFN(const uint8_t *typeLayout, size_t &offset,
                                      bool resolved, uint8_t *addr,
                                      uintptr_t &addrOffset) {
  GetEnumTagFn getEnumTag;
  if (resolved) {
    getEnumTag = readBytes<GetEnumTagFn>(typeLayout, offset);
  } else {
    getEnumTag = readRelativeFunctionPointer<GetEnumTagFn>(typeLayout, offset);
  }

  unsigned enumTag = getEnumTag(addr + addrOffset);

  if (enumTag == 0) {
    offset += sizeof(size_t) * 2;
  } else {
    auto refCountBytes = readBytes<size_t>(typeLayout, offset);
    auto skip = readBytes<size_t>(typeLayout, offset);
    offset += refCountBytes;
    addrOffset += skip;
  }
}

template <typename Handler, typename... Params>
static void handleMultiPayloadEnumFN(const Metadata *metadata,
                                     const uint8_t *typeLayout, size_t &offset,
                                     bool resolved, uintptr_t &addrOffset,
                                     uint8_t *addr, Params... params) {
  GetEnumTagFn getEnumTag;
  if (resolved) {
    getEnumTag = readBytes<GetEnumTagFn>(typeLayout, offset);
  } else {
    getEnumTag = readRelativeFunctionPointer<GetEnumTagFn>(typeLayout, offset);
  }

  size_t numCases = readBytes<size_t>(typeLayout, offset);
  size_t refCountBytes = readBytes<size_t>(typeLayout, offset);
  size_t enumSize = readBytes<size_t>(typeLayout, offset);

  unsigned enumTag = getEnumTag(addr + addrOffset);

  if (enumTag < numCases) {
    size_t nestedOffset = offset + (enumTag * sizeof(size_t));
    size_t refCountOffset = readBytes<size_t>(typeLayout, nestedOffset);
    nestedOffset = offset + (numCases * sizeof(size_t)) + refCountOffset;

    uintptr_t nestedAddrOffset = addrOffset;
    handleRefCounts<0, Handler>(metadata, typeLayout, nestedOffset,
                                nestedAddrOffset, addr,
                                std::forward<Params>(params)...);
  }

  offset += refCountBytes + (numCases * sizeof(size_t));
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

  static inline void handleSinglePayloadEnumSimple(const uint8_t *typeLayout,
                                                   size_t &offset,
                                                   uintptr_t &addrOffset,
                                                   uint8_t *addr) {
    ::handleSinglePayloadEnumSimple(typeLayout, offset, addr, addrOffset);
  }

  static inline void handleSinglePayloadEnumFN(const uint8_t *typeLayout,
                                               size_t &offset, bool resolved,
                                               uintptr_t &addrOffset,
                                               uint8_t *addr) {
    ::handleSinglePayloadEnumFN(typeLayout, offset, resolved, addr, addrOffset);
  }

  static inline void handleMultiPayloadEnumFN(const Metadata *metadata,
                                              const uint8_t *typeLayout,
                                              size_t &offset, bool resolved,
                                              uintptr_t &addrOffset,
                                              uint8_t *addr) {
    ::handleMultiPayloadEnumFN<DestroyHandler>(metadata, typeLayout, offset,
                                               resolved, addrOffset, addr);
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

  static inline void handleSinglePayloadEnumSimple(const uint8_t *typeLayout,
                                                   size_t &offset,
                                                   uintptr_t &addrOffset,
                                                   uint8_t *dest,
                                                   uint8_t *src) {
    ::handleSinglePayloadEnumSimple(typeLayout, offset, src, addrOffset);
  }

  static inline void handleSinglePayloadEnumFN(const uint8_t *typeLayout,
                                               size_t &offset, bool resolved,
                                               uintptr_t &addrOffset,
                                               uint8_t *dest, uint8_t *src) {
    ::handleSinglePayloadEnumFN(typeLayout, offset, resolved, src, addrOffset);
  }

  static inline void handleMultiPayloadEnumFN(const Metadata *metadata,
                                              const uint8_t *typeLayout,
                                              size_t &offset, bool resolved,
                                              uintptr_t &addrOffset,
                                              uint8_t *dest, uint8_t *src) {
    ::handleMultiPayloadEnumFN<CopyHandler>(metadata, typeLayout, offset,
                                            resolved, addrOffset, dest, src);
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

  static inline void handleSinglePayloadEnumSimple(const uint8_t *typeLayout,
                                                   size_t &offset,
                                                   uintptr_t &addrOffset,
                                                   uint8_t *dest,
                                                   uint8_t *src) {
    ::handleSinglePayloadEnumSimple(typeLayout, offset, src, addrOffset);
  }

  static inline void handleSinglePayloadEnumFN(const uint8_t *typeLayout,
                                               size_t &offset, bool resolved,
                                               uintptr_t &addrOffset,
                                               uint8_t *dest, uint8_t *src) {
    ::handleSinglePayloadEnumFN(typeLayout, offset, resolved, src, addrOffset);
  }

  static inline void handleMultiPayloadEnumFN(const Metadata *metadata,
                                              const uint8_t *typeLayout,
                                              size_t &offset, bool resolved,
                                              uintptr_t &addrOffset,
                                              uint8_t *dest, uint8_t *src) {
    ::handleMultiPayloadEnumFN<TakeHandler>(metadata, typeLayout, offset,
                                            resolved, addrOffset, dest, src);
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

void swift::swift_resolve_resilientAccessors(uint8_t *layoutStr,
                                             size_t layoutStrOffset,
                                             const uint8_t *fieldLayoutStr,
                                             const Metadata *fieldType) {
  size_t i = layoutStringHeaderSize;
  while (true) {
    size_t currentOffset = i;
    uint64_t size = readBytes<uint64_t>(fieldLayoutStr, i);
    RefCountingKind tag = (RefCountingKind)(size >> 56);
    size &= ~(0xffULL << 56);

    switch (tag) {
    case RefCountingKind::End:
      return;
    case RefCountingKind::Resilient: {
      auto *type = getResilientTypeMetadata(fieldType, fieldLayoutStr, i);
      size_t writeOffset = layoutStrOffset + currentOffset -
                           layoutStringHeaderSize;
      uint64_t tagAndOffset =
          (((uint64_t)RefCountingKind::Metatype) << 56) | size;
      writeBytes(layoutStr, writeOffset, tagAndOffset);
      writeBytes(layoutStr, writeOffset, type);
      break;
    }
    case RefCountingKind::Metatype:
      i += sizeof(uintptr_t);
      break;
    case RefCountingKind::SinglePayloadEnumSimple:
      i += (3 * sizeof(uint64_t)) + (4 * sizeof(size_t));
      break;

    case RefCountingKind::SinglePayloadEnumFN: {
      auto getEnumTag =
          readRelativeFunctionPointer<GetEnumTagFn>(fieldLayoutStr, i);
      size_t writeOffset =
          layoutStrOffset + currentOffset - layoutStringHeaderSize;
      uint64_t tagAndOffset =
          (((uint64_t)RefCountingKind::SinglePayloadEnumFNResolved) << 56) |
          size;
      writeBytes(layoutStr, writeOffset, tagAndOffset);
      writeBytes(layoutStr, writeOffset, getEnumTag);
      i += 2 * sizeof(size_t);
      break;
    }

    case RefCountingKind::SinglePayloadEnumFNResolved:
      i += 3 * sizeof(size_t);
      break;

    case RefCountingKind::MultiPayloadEnumFN: {
      auto getEnumTag =
          readRelativeFunctionPointer<GetEnumTagFn>(fieldLayoutStr, i);
      size_t writeOffset =
          layoutStrOffset + currentOffset - layoutStringHeaderSize;
      uint64_t tagAndOffset =
          (((uint64_t)RefCountingKind::MultiPayloadEnumFNResolved) << 56) |
          size;
      writeBytes(layoutStr, writeOffset, tagAndOffset);
      writeBytes(layoutStr, writeOffset, getEnumTag);

      size_t numCases = readBytes<size_t>(fieldLayoutStr, i);
      // skip ref count bytes
      i += sizeof(size_t);

      size_t casesBeginOffset =
          layoutStrOffset + i + (numCases * sizeof(size_t));

      for (size_t j = 0; j < numCases; j++) {
        size_t caseOffset = readBytes<size_t>(fieldLayoutStr, i);
        const uint8_t *caseLayoutString =
            fieldLayoutStr + i + (numCases * sizeof(size_t)) + caseOffset;
        swift_resolve_resilientAccessors(layoutStr,
                                         casesBeginOffset + caseOffset,
                                         caseLayoutString, fieldType);
      }
      break;
    }

    case RefCountingKind::MultiPayloadEnumFNResolved: {
      // skip function pointer
      i += sizeof(uintptr_t);
      size_t numCases = readBytes<size_t>(fieldLayoutStr, i);
      size_t refCountBytes = readBytes<size_t>(fieldLayoutStr, i);
      // skip enum size, offsets and ref counts
      i += sizeof(size_t) + (numCases * sizeof(size_t)) + refCountBytes;
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

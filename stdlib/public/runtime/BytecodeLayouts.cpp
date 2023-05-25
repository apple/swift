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
T readBytes(const uint8_t *typeLayout, size_t &i) {
  T returnVal = *(const T *)(typeLayout + i);
  i += sizeof(T);
  return returnVal;
}

/// Given a pointer, a value, and an offset, write the value at the given
/// offset in big-endian order
template <typename T>
void writeBytes(uint8_t *typeLayout, size_t i, T value) {
  *((T*)(typeLayout + i)) = value;
}

Metadata *getExistentialTypeMetadata(OpaqueValue *object) {
  return reinterpret_cast<Metadata**>(object)[NumWords_ValueBuffer];
}

typedef Metadata* (*MetadataAccessor)(const Metadata* const *);

const Metadata *getResilientTypeMetadata(const Metadata* metadata,
                                         const uint8_t *layoutStr,
                                         size_t &offset) {
  auto absolute = layoutStr + offset;
  auto relativeOffset = (uintptr_t)(intptr_t)(int32_t)readBytes<intptr_t>(layoutStr, offset);
  MetadataAccessor fn;

#if SWIFT_PTRAUTH
  fn = (MetadataAccessor)ptrauth_sign_unauthenticated(
      (void *)((uintptr_t)absolute + relativeOffset),
      ptrauth_key_function_pointer, 0);
#else
  fn = (MetadataAccessor)((uintptr_t)absolute + relativeOffset);
#endif

  return fn(metadata->getGenericArgs());
}

typedef void (*DestrFn)(void*);

struct DestroyFuncAndMask {
  DestrFn fn;
  bool isIndirect;
};

void skipDestroy(void* ignore) { }

void existential_destroy(OpaqueValue* object) {
  auto* metadata = getExistentialTypeMetadata(object);
  if (metadata->getValueWitnesses()->isValueInline()) {
    metadata->vw_destroy(object);
  } else {
    swift_release(*(HeapObject**)object);
  }
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

uint64_t readTagBytes(uint8_t *addr, uint8_t byteCount) {
  switch (byteCount) {
  case 1:
    return addr[0];
  case 2:
    return ((uint16_t *)addr)[0];
  case 4:
    return ((uint32_t *)addr)[0];
  case 8:
    return ((uint64_t *)addr)[0];
  default:
    swift_unreachable("Unsupported tag byte length.");
  }
}

void handleSinglePayloadEnumSimple(const uint8_t *typeLayout, size_t &offset,
                                   uint8_t *addr, size_t &addrOffset) {
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

extern "C" void
swift_generic_destroy(swift::OpaqueValue *address, const Metadata *metadata) {
  uint8_t *addr = (uint8_t *)address;

  const uint8_t *typeLayout = metadata->getLayoutString();

  size_t offset = layoutStringHeaderSize;
  uintptr_t addrOffset = 0;

  while (true) {
    uint64_t skip = readBytes<uint64_t>(typeLayout, offset);
    auto tag = static_cast<RefCountingKind>(skip >> 56);
    skip &= ~(0xffULL << 56);
    addrOffset += skip;

    if (SWIFT_UNLIKELY(tag == RefCountingKind::End)) {
      return;
    } else if (SWIFT_UNLIKELY(tag == RefCountingKind::Metatype)) {
      auto *type = readBytes<const Metadata*>(typeLayout, offset);
      type->vw_destroy((OpaqueValue *)(addr + addrOffset));
    } else if (SWIFT_UNLIKELY(tag == RefCountingKind::Resilient)) {
      auto *type = getResilientTypeMetadata(metadata, typeLayout, offset);
      type->vw_destroy((OpaqueValue *)(addr + addrOffset));
    } else if (SWIFT_UNLIKELY(tag ==
                              RefCountingKind::SinglePayloadEnumSimple)) {
      handleSinglePayloadEnumSimple(typeLayout, offset, addr, addrOffset);
    } else {
      const auto &destroyFunc = destroyTable[static_cast<uint8_t>(tag)];
      if (SWIFT_LIKELY(destroyFunc.isIndirect)) {
        destroyFunc.fn(
            (void *)((*(uintptr_t *)(addr + addrOffset))));
      } else {
        destroyFunc.fn(((void *)(addr + addrOffset)));
      }
    }
  }
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

extern "C" swift::OpaqueValue *
swift_generic_initWithCopy(swift::OpaqueValue *dest, swift::OpaqueValue *src,
                           const Metadata *metadata) {
  uintptr_t addrOffset = 0;
  const uint8_t *typeLayout = metadata->getLayoutString();

  size_t size = metadata->vw_size();

  auto offset = layoutStringHeaderSize;

  memcpy(dest, src, size);

  while (true) {
    uint64_t skip = readBytes<uint64_t>(typeLayout, offset);
    auto tag = static_cast<RefCountingKind>(skip >> 56);
    skip &= ~(0xffULL << 56);
    addrOffset += skip;

    if (SWIFT_UNLIKELY(tag == RefCountingKind::End)) {
      return dest;
    } else if (SWIFT_UNLIKELY(tag == RefCountingKind::Metatype)) {
      auto *type = readBytes<const Metadata*>(typeLayout, offset);
      type->vw_initializeWithCopy((OpaqueValue*)((uintptr_t)dest + addrOffset),
                                  (OpaqueValue*)((uintptr_t)src + addrOffset));
    } else if (SWIFT_UNLIKELY(tag == RefCountingKind::Resilient)) {
      auto *type = getResilientTypeMetadata(metadata, typeLayout, offset);
      type->vw_initializeWithCopy((OpaqueValue*)((uintptr_t)dest + addrOffset),
                                  (OpaqueValue*)((uintptr_t)src + addrOffset));
    } else if (SWIFT_UNLIKELY(tag ==
                              RefCountingKind::SinglePayloadEnumSimple)) {
      handleSinglePayloadEnumSimple(typeLayout, offset, (uint8_t *)src,
                                    addrOffset);
    } else {
      const auto &retainFunc = retainTable[static_cast<uint8_t>(tag)];
      if (SWIFT_LIKELY(retainFunc.isSingle)) {
        ((RetainFn)retainFunc.fn)(*(void**)(((uintptr_t)dest + addrOffset)));
      } else {
        ((CopyInitFn)retainFunc.fn)((void*)((uintptr_t)dest + addrOffset),
                                    (void*)((uintptr_t)src + addrOffset));
      }
    }
  }
}

extern "C" swift::OpaqueValue *
swift_generic_initWithTake(swift::OpaqueValue *dest, swift::OpaqueValue *src,
                           const Metadata *metadata) {
  const uint8_t *typeLayout = metadata->getLayoutString();
  size_t size = metadata->vw_size();

  memcpy(dest, src, size);

  if (SWIFT_LIKELY(metadata->getValueWitnesses()->isBitwiseTakable())) {
    return dest;
  }

  auto offset = layoutStringHeaderSize;
  uintptr_t addrOffset = 0;

  while (true) {
    uint64_t skip = readBytes<uint64_t>(typeLayout, offset);
    auto tag = static_cast<RefCountingKind>(skip >> 56);
    skip &= ~(0xffULL << 56);
    addrOffset += skip;

    switch (tag) {
    case RefCountingKind::UnknownWeak:
      swift_unknownObjectWeakTakeInit(
          (WeakReference*)((uintptr_t)dest + addrOffset),
          (WeakReference*)((uintptr_t)src + addrOffset));
      break;
    case RefCountingKind::Metatype: {
      auto *type = readBytes<const Metadata*>(typeLayout, offset);
      if (SWIFT_UNLIKELY(!type->getValueWitnesses()->isBitwiseTakable())) {
        type->vw_initializeWithTake(
            (OpaqueValue*)((uintptr_t)dest + addrOffset),
            (OpaqueValue*)((uintptr_t)src + addrOffset));
      }
      break;
    }
    case RefCountingKind::Existential: {
      auto *type = getExistentialTypeMetadata(
          (OpaqueValue*)((uintptr_t)src + addrOffset));
      if (SWIFT_UNLIKELY(!type->getValueWitnesses()->isBitwiseTakable())) {
        type->vw_initializeWithTake(
            (OpaqueValue*)((uintptr_t)dest + addrOffset),
            (OpaqueValue*)((uintptr_t)src + addrOffset));
      }
      break;
    }
    case RefCountingKind::Resilient: {
      auto *type = getResilientTypeMetadata(metadata, typeLayout, offset);
      if (SWIFT_UNLIKELY(!type->getValueWitnesses()->isBitwiseTakable())) {
        type->vw_initializeWithTake((OpaqueValue*)((uintptr_t)dest + addrOffset),
                                    (OpaqueValue*)((uintptr_t)src + addrOffset));
      }
      break;
    }

    case RefCountingKind::SinglePayloadEnumSimple: {
      handleSinglePayloadEnumSimple(typeLayout, offset, (uint8_t *)src,
                                    addrOffset);
      break;
    }

    case RefCountingKind::End:
      return dest;
    default:
      break;
    }
  }

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

void swift::swift_resolve_resilientAccessors(
    uint8_t *layoutStr, size_t layoutStrOffset, const uint8_t *fieldLayoutStr,
    size_t refCountBytes, const Metadata *fieldType) {
  size_t i = layoutStringHeaderSize;
  while (i < (layoutStringHeaderSize + refCountBytes)) {
    size_t currentOffset = i;
    uint64_t size = readBytes<uint64_t>(fieldLayoutStr, i);
    RefCountingKind tag = (RefCountingKind)(size >> 56);
    size &= ~(0xffULL << 56);

    switch (tag) {
    case RefCountingKind::Resilient: {
      auto *type = getResilientTypeMetadata(fieldType, fieldLayoutStr,
                                            i);
      uint8_t *curPos = (layoutStr + layoutStrOffset + currentOffset - layoutStringHeaderSize);
      *((uint64_t*)curPos) =
          (((uint64_t)RefCountingKind::Metatype) << 56) | size;
      *((Metadata const* *)(curPos + sizeof(uint64_t))) = type;
      break;
    }
    case RefCountingKind::Metatype:
      i += sizeof(uintptr_t);
      break;
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

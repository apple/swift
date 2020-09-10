//===--- KeyPaths.cpp - Key path helper symbols ---------------------------===//
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

#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Metadata.h"
#include <cstdint>
#include <cstring>

using namespace swift;

SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
void swift_copyKeyPathTrivialIndices(const void *src, void *dest, size_t bytes) {
  memcpy(dest, src, bytes);
}

SWIFT_CC(swift)
static bool equateGenericArguments(const void *a, const void *b, size_t bytes) {
  // Generic arguments can't affect equality, since an equivalent key path may
  // have been formed in a fully concrete context without capturing generic
  // arguments.
  return true;
}

SWIFT_CC(swift)
static intptr_t hashGenericArguments(const void *src, size_t bytes) {
  // Generic arguments can't affect equality, since an equivalent key path may
  // have been formed in a fully concrete context without capturing generic
  // arguments. The implementation recognizes a hash value return of '0' as
  // "no effect on the hash".
  return 0;
}

struct KeyPathGenericWitnessTable {
  void *destroy;
  SWIFT_CC(swift) void (* __ptrauth_swift_runtime_function_entry_with_key(swift::SpecialPointerAuthDiscriminators::KeyPathCopy) copy)(const void *src, void *dest, size_t bytes);
  SWIFT_CC(swift) bool (* __ptrauth_swift_runtime_function_entry_with_key(swift::SpecialPointerAuthDiscriminators::KeyPathEquals) equals)(const void *, const void *, size_t);
  SWIFT_CC(swift) intptr_t (* __ptrauth_swift_runtime_function_entry_with_key(swift::SpecialPointerAuthDiscriminators::KeyPathHash) hash)(const void *src, size_t bytes);
};

/// A prefab witness table for computed key path components that only include
/// captured generic arguments.
SWIFT_RUNTIME_EXPORT
KeyPathGenericWitnessTable swift_keyPathGenericWitnessTable = {
  nullptr,
  swift_copyKeyPathTrivialIndices,
  equateGenericArguments,
  hashGenericArguments,
};

/****************************************************************************/
/** Projection functions ****************************************************/
/****************************************************************************/

namespace {
  struct AddrAndOwner {
    OpaqueValue *Addr;
    HeapObject *Owner;
  };
}

// These functions are all implemented in the stdlib.  Their type
// parameters are passed implicitly in the isa of the key path.

extern "C"
SWIFT_CC(swift) void
swift_getAtKeyPath(SWIFT_INDIRECT_RESULT void *result,
                   const OpaqueValue *root, void *keyPath);

extern "C"
SWIFT_CC(swift) AddrAndOwner
_swift_modifyAtWritableKeyPath_impl(OpaqueValue *root, void *keyPath);

extern "C"
SWIFT_CC(swift) AddrAndOwner
_swift_modifyAtReferenceWritableKeyPath_impl(const OpaqueValue *root,
                                             void *keyPath);

namespace {
  struct YieldOnceTemporary {
    const Metadata *Type;

    // Yield-once buffers can't be memcpy'ed, so it doesn't matter that
    // isValueInline() returns false for non-bitwise-takable types --- but
    // it doesn't hurt, either.
    ValueBuffer Buffer;

    YieldOnceTemporary(const Metadata *type) : Type(type) {}

    static OpaqueValue *allocateIn(const Metadata *type,
                                   YieldOnceBuffer *buffer) {
      auto *temp =
        new (reinterpret_cast<void*>(buffer)) YieldOnceTemporary(type);
      return type->allocateBufferIn(&temp->Buffer);
    }

    static void destroyAndDeallocateIn(YieldOnceBuffer *buffer) {
      auto *temp = reinterpret_cast<YieldOnceTemporary*>(buffer);
      temp->Type->vw_destroy(temp->Type->projectBufferFrom(&temp->Buffer));
      temp->Type->deallocateBufferIn(&temp->Buffer);
    }
  };

  static_assert(sizeof(YieldOnceTemporary) <= sizeof(YieldOnceBuffer) &&
                alignof(YieldOnceTemporary) <= alignof(YieldOnceBuffer),
                "temporary doesn't fit in a YieldOnceBuffer");
}

static SWIFT_CC(swift)
void _destroy_temporary_continuation(YieldOnceBuffer *buffer, bool forUnwind) {
  YieldOnceTemporary::destroyAndDeallocateIn(buffer);
}

YieldOnceResult<const OpaqueValue*>
swift::swift_readAtKeyPath(YieldOnceBuffer *buffer,
                           const OpaqueValue *root, void *keyPath) {
  // The Value type parameter is passed in the class of the key path object.
  // KeyPath is a native class, so we can just load its metadata directly
  // even on ObjC-interop targets.
  const Metadata *keyPathType = static_cast<HeapObject*>(keyPath)->metadata;

  // To find the generic arguments, we just have to find the class-specific
  // data section of the class; the generic arguments are always at the start
  // of that.
  //
  // We're within KeyPath's resilience domain, let's just hard-code an offset
  // to the start of KeyPath's class-specific data.
  auto keyPathGenericArgs =
    reinterpret_cast<const Metadata * const *>(
      reinterpret_cast<const char*>(keyPathType) + 0 /* XXX TODO FIXME */);
  const Metadata *valueTy = keyPathGenericArgs[1];

  // Allocate the buffer.
  auto result = YieldOnceTemporary::allocateIn(valueTy, buffer);

  // Read into the buffer.
  swift_getAtKeyPath(result, root, keyPath);

  // Return a continuation that destroys the value in the buffer
  // and deallocates it.
  return { swift_ptrauth_sign_opaque_read_resume_function(
             &_destroy_temporary_continuation, buffer),
           result };
}

static SWIFT_CC(swift)
void _release_owner_continuation(YieldOnceBuffer *buffer, bool forUnwind) {
  swift_unknownObjectRelease(buffer->Data[0]);
}

YieldOnceResult<OpaqueValue*>
swift::swift_modifyAtWritableKeyPath(YieldOnceBuffer *buffer,
                                     OpaqueValue *root, void *keyPath) {
  auto addrAndOwner =
    _swift_modifyAtWritableKeyPath_impl(root, keyPath);
  buffer->Data[0] = addrAndOwner.Owner;

  return { swift_ptrauth_sign_opaque_modify_resume_function(
             &_release_owner_continuation, buffer),
           addrAndOwner.Addr };
}

YieldOnceResult<OpaqueValue*>
swift::swift_modifyAtReferenceWritableKeyPath(YieldOnceBuffer *buffer,
                                              const OpaqueValue *root,
                                              void *keyPath) {
  auto addrAndOwner =
    _swift_modifyAtReferenceWritableKeyPath_impl(root, keyPath);
  buffer->Data[0] = addrAndOwner.Owner;

  return { swift_ptrauth_sign_opaque_modify_resume_function(
             &_release_owner_continuation, buffer),
           addrAndOwner.Addr };
}

//===--- ErrorObject.h - Cocoa-interoperable recoverable error object -----===//
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
// This implements the object representation of the standard Error
// protocol type, which represents recoverable errors in the language. This
// implementation is designed to interoperate efficiently with Cocoa libraries
// by:
// - allowing for NSError and CFError objects to "toll-free bridge" to
//   Error existentials, which allows for cheap Cocoa to Swift interop
// - allowing a native Swift error to lazily "become" an NSError when
//   passed into Cocoa, allowing for cheap Swift to Cocoa interop
//
//===----------------------------------------------------------------------===//

#ifndef __SWIFT_RUNTIME_ERROROBJECT_H__
#define __SWIFT_RUNTIME_ERROROBJECT_H__

#include "swift/Runtime/Metadata.h"
#include "swift/Runtime/HeapObject.h"
#include "SwiftHashableSupport.h"

#include "llvm/Support/Compiler.h"

#include <atomic>
#if SWIFT_OBJC_INTEROP
# include <CoreFoundation/CoreFoundation.h>
# include <objc/objc.h>
#endif

namespace swift {

#if SWIFT_OBJC_INTEROP

// Copied from CoreFoundation/CFRuntime.h.
struct CFRuntimeBase {
  void *opaque1;
  void *opaque2;
};

/// When ObjC interop is enabled, SwiftError uses an NSError-layout-compatible
/// header.
struct SwiftErrorHeader {
  // CFError has a CF refcounting header. NSError reserves a word after the
  // 'isa' in order to be layout-compatible.
  CFRuntimeBase base;
  // The NSError part of the object is lazily initialized, so we need atomic
  // semantics.
  std::atomic<CFIndex> code;
  std::atomic<CFStringRef> domain;
  std::atomic<CFDictionaryRef> userInfo;
};

#else

/// When ObjC interop is disabled, SwiftError uses a normal Swift heap object
/// header.
using SwiftErrorHeader = HeapObject;

#endif

/// The layout of the Swift Error box.
struct SwiftError : SwiftErrorHeader {
  // By inheriting OpaqueNSError, the SwiftError structure reserves enough
  // space within itself to lazily emplace an NSError instance, and gets
  // Core Foundation's refcounting scheme.

  /// The type of Swift error value contained in the box.
  /// This member is only available for native Swift errors.
  const Metadata *type;

  /// The witness table for `Error` conformance.
  /// This member is only available for native Swift errors.
  const WitnessTable *errorConformance;

#if SWIFT_OBJC_INTEROP
  /// The base type that introduces the `Hashable` conformance.
  /// This member is only available for native Swift errors.
  /// This member is lazily-initialized.
  /// Instead of using it directly, call `getHashableBaseType()`.
  mutable std::atomic<const Metadata *> hashableBaseType;

  /// The witness table for `Hashable` conformance.
  /// This member is only available for native Swift errors.
  /// This member is lazily-initialized.
  /// Instead of using it directly, call `getHashableConformance()`.
  mutable std::atomic<const hashable_support::HashableWitnessTable *> hashableConformance;
#endif

  /// Get a pointer to the value contained inside the indirectly-referenced
  /// box reference.
  static const OpaqueValue *getIndirectValue(const SwiftError * const *ptr) {
    // If the box is a bridged NSError, then the box's address is itself the
    // value.
    if ((*ptr)->isPureNSError())
      return reinterpret_cast<const OpaqueValue *>(ptr);
    return (*ptr)->getValue();
  }
  static OpaqueValue *getIndirectValue(SwiftError * const *ptr) {
    return const_cast<OpaqueValue *>(getIndirectValue(
                                  const_cast<const SwiftError * const *>(ptr)));
  }
  
  /// Get a pointer to the value, which is tail-allocated after
  /// the fixed header.
  const OpaqueValue *getValue() const {
    // If the box is a bridged NSError, then the box's address is itself the
    // value. We can't provide an address for that; getIndirectValue must be
    // used if we haven't established this as an NSError yet..
    assert(!isPureNSError());
  
    auto baseAddr = reinterpret_cast<uintptr_t>(this + 1);
    // Round up to the value's alignment.
    unsigned alignMask = type->getValueWitnesses()->getAlignmentMask();
    baseAddr = (baseAddr + alignMask) & ~(uintptr_t)alignMask;
    return reinterpret_cast<const OpaqueValue *>(baseAddr);
  }
  OpaqueValue *getValue() {
    return const_cast<OpaqueValue*>(
             const_cast<const SwiftError *>(this)->getValue());
  }
  
#if SWIFT_OBJC_INTEROP
  // True if the object is really an NSError or CFError instance.
  // The type and errorConformance fields don't exist in an NSError.
  bool isPureNSError() const;
#else
  bool isPureNSError() const { return false; }
#endif
  
#if SWIFT_OBJC_INTEROP
  /// Get the type of the contained value.
  const Metadata *getType() const;
  /// Get the Error protocol witness table for the contained type.
  const WitnessTable *getErrorConformance() const;
#else
  /// Get the type of the contained value.
  const Metadata *getType() const { return type; }
  /// Get the Error protocol witness table for the contained type.
  const WitnessTable *getErrorConformance() const { return errorConformance; }
#endif

#if SWIFT_OBJC_INTEROP
  /// Get the base type that conforms to `Hashable`.
  /// Returns NULL if the type does not conform.
  const Metadata *getHashableBaseType() const;

  /// Get the `Hashable` protocol witness table for the contained type.
  /// Returns NULL if the type does not conform.
  const hashable_support::HashableWitnessTable *getHashableConformance() const;
#endif

  // Don't copy or move, please.
  SwiftError(const SwiftError &) = delete;
  SwiftError(SwiftError &&) = delete;
  SwiftError &operator=(const SwiftError &) = delete;
  SwiftError &operator=(SwiftError &&) = delete;
};

/// Allocate a catchable error object.
///
/// If value is nonnull, it should point to a value of \c type, which will be
/// copied (or taken if \c isTake is true) into the newly-allocated error box.
/// If value is null, the box's contents will be left uninitialized, and
/// \c isTake should be false.
SWIFT_CC SWIFT_RUNTIME_STDLIB_API
BoxPair swift_allocError(const Metadata *type,
                         const WitnessTable *errorConformance,
                         OpaqueValue *value, bool isTake);
  
/// Deallocate an error object whose contained object has already been
/// destroyed.
SWIFT_RUNTIME_STDLIB_API
void swift_deallocError(SwiftError *error, const Metadata *type);

struct ErrorValueResult {
  const OpaqueValue *value;
  const Metadata *type;
  const WitnessTable *errorConformance;
};

/// Extract a pointer to the value, the type metadata, and the Error
/// protocol witness from an error object.
///
/// The "scratch" pointer should point to an uninitialized word-sized
/// temporary buffer. The implementation may write a reference to itself to
/// that buffer if the error object is a toll-free-bridged NSError instead of
/// a native Swift error, in which case the object itself is the "boxed" value.
SWIFT_RUNTIME_STDLIB_API
void swift_getErrorValue(const SwiftError *errorObject,
                         void **scratch,
                         ErrorValueResult *out);

/// Retain and release SwiftError boxes.
SWIFT_RUNTIME_STDLIB_API
SwiftError *swift_errorRetain(SwiftError *object);
SWIFT_RUNTIME_STDLIB_API
void swift_errorRelease(SwiftError *object);

/// Breakpoint hook for debuggers.
SWIFT_CC SWIFT_RUNTIME_STDLIB_API
void swift_willThrow(SWIFT_CONTEXT void *unused,
                     SWIFT_ERROR_RESULT SwiftError **object);

/// Halt in response to an error.
SWIFT_CC SWIFT_RUNTIME_STDLIB_API LLVM_ATTRIBUTE_NORETURN
void swift_errorInMain(SwiftError *object);
SWIFT_CC SWIFT_RUNTIME_STDLIB_API LLVM_ATTRIBUTE_NORETURN
void swift_unexpectedError(SwiftError *object,
                           OpaqueValue *filenameStart,
                           long filenameLength,
                           bool isAscii,
                           unsigned long line);

#if SWIFT_OBJC_INTEROP

/// Initialize an Error box to make it usable as an NSError instance.
///
/// errorObject is assumed to be passed at +1 and consumed in this function.
SWIFT_CC SWIFT_RUNTIME_STDLIB_SPI
id _swift_stdlib_bridgeErrorToNSError(SwiftError *errorObject);

/// Attempt to dynamically cast an NSError object to a Swift ErrorType
/// implementation using the _ObjectiveCBridgeableErrorType protocol or by
/// putting it directly into an Error existential.
bool tryDynamicCastNSErrorObjectToValue(HeapObject *object,
                                        OpaqueValue *dest,
                                        const Metadata *destType,
                                        DynamicCastFlags flags);

/// Attempt to dynamically cast an NSError instance to a Swift ErrorType
/// implementation using the _ObjectiveCBridgeableErrorType protocol or by
/// putting it directly into an Error existential.
///
/// srcType must be some kind of class metadata.
bool tryDynamicCastNSErrorToValue(OpaqueValue *dest,
                                  OpaqueValue *src,
                                  const Metadata *srcType,
                                  const Metadata *destType,
                                  DynamicCastFlags flags);

/// Get the NSError Objective-C class.
Class getNSErrorClass();

/// Get the NSError metadata.
const Metadata *getNSErrorMetadata();

/// Find the witness table for the conformance of the given type to the
/// Error protocol, or return nullptr if it does not conform.
const WitnessTable *findErrorWitness(const Metadata *srcType);

/// Dynamically cast a value whose conformance to the Error protocol is known
/// into an NSError instance.
id dynamicCastValueToNSError(OpaqueValue *src,
                             const Metadata *srcType,
                             const WitnessTable *srcErrorWitness,
                             DynamicCastFlags flags);

#endif

SWIFT_RUNTIME_STDLIB_SPI
const size_t _swift_lldb_offsetof_SwiftError_typeMetadata;

SWIFT_RUNTIME_STDLIB_SPI
const size_t _swift_lldb_sizeof_SwiftError;

} // namespace swift

#if SWIFT_OBJC_INTEROP
// internal func _getErrorEmbeddedNSErrorIndirect<T : Error>(
//   _ x: UnsafePointer<T>) -> AnyObject?
#define getErrorEmbeddedNSErrorIndirect \
  MANGLE_SYM(s32_getErrorEmbeddedNSErrorIndirectyyXlSgSPyxGs0B0RzlF)
SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
id getErrorEmbeddedNSErrorIndirect(const swift::OpaqueValue *error,
                                   const swift::Metadata *T,
                                   const swift::WitnessTable *Error);
#endif

#endif

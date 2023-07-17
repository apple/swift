//===--- Casting.h - Swift type-casting runtime support ---------*- C++ -*-===//
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
// Swift runtime functions for casting values.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_RUNTIME_CASTING_H
#define SWIFT_RUNTIME_CASTING_H

#include "swift/Runtime/Metadata.h"

namespace swift {

/// Perform a checked dynamic cast of a value to a target type.
///
/// \param dest A buffer into which to write the destination value.
/// In all cases, this will be left uninitialized if the cast fails.
///
/// \param src Pointer to the source value to cast.  This may be left
///   uninitialized after the operation, depending on the flags.
///
/// \param targetType The type to which we are casting.
///
/// \param srcType The static type of the source value.
///
/// \param flags Flags to control the operation.
///
/// \return true if the cast succeeded. Depending on the flags,
///   swift_dynamicCast may fail rather than return false.
SWIFT_RUNTIME_EXPORT
bool
swift_dynamicCast(OpaqueValue *dest, OpaqueValue *src,
                  const Metadata *srcType,
                  const Metadata *targetType,
                  DynamicCastFlags flags);

/// Checked dynamic cast to a Swift class type.
///
/// \param object The object to cast.
/// \param targetType The type to which we are casting, which is known to be
/// a Swift class type.
///
/// \returns the object if the cast succeeds, or null otherwise.
SWIFT_RUNTIME_EXPORT
const void *
swift_dynamicCastClass(const void *object, const ClassMetadata *targetType);

/// Unconditional, checked dynamic cast to a Swift class type.
///
/// Aborts if the object isn't of the target type.
///
/// \param object The object to cast.
/// \param targetType The type to which we are casting, which is known to be
/// a Swift class type.
/// \param file The source filename from which to report failure. May be null.
/// \param line The source line from which to report failure.
/// \param column The source column from which to report failure.
///
/// \returns the object.
SWIFT_RUNTIME_EXPORT
const void *
swift_dynamicCastClassUnconditional(const void *object,
                                    const ClassMetadata *targetType,
                                    const char *file, unsigned line, unsigned column);

#if SWIFT_OBJC_INTEROP
/// Checked Objective-C-style dynamic cast to a class type.
///
/// \param object The object to cast, or nil.
/// \param targetType The type to which we are casting, which is known to be
/// a class type, but not necessarily valid type metadata.
///
/// \returns the object if the cast succeeds, or null otherwise.
SWIFT_RUNTIME_EXPORT
const void *
swift_dynamicCastObjCClass(const void *object, const ClassMetadata *targetType);

/// Checked dynamic cast to a foreign class type.
///
/// \param object The object to cast, or nil.
/// \param targetType The type to which we are casting, which is known to be
/// a foreign class type.
///
/// \returns the object if the cast succeeds, or null otherwise.
SWIFT_RUNTIME_EXPORT
const void *
swift_dynamicCastForeignClass(const void *object,
                              const ForeignClassMetadata *targetType);

/// Unconditional, checked, Objective-C-style dynamic cast to a class
/// type.
///
/// Aborts if the object isn't of the target type.
/// Note that unlike swift_dynamicCastClassUnconditional, this does not abort
/// if the object is 'nil'.
///
/// \param object The object to cast, or nil.
/// \param targetType The type to which we are casting, which is known to be
/// a class type, but not necessarily valid type metadata.
/// \param file The source filename from which to report failure. May be null.
/// \param line The source line from which to report failure.
/// \param column The source column from which to report failure.
///
/// \returns the object.
SWIFT_RUNTIME_EXPORT
const void *
swift_dynamicCastObjCClassUnconditional(const void *object,
                                        const ClassMetadata *targetType,
                                        const char *file, unsigned line, unsigned column);

/// Unconditional, checked dynamic cast to a foreign class type.
///
/// \param object The object to cast, or nil.
/// \param targetType The type to which we are casting, which is known to be
/// a foreign class type.
/// \param file The source filename from which to report failure. May be null.
/// \param line The source line from which to report failure.
/// \param column The source column from which to report failure.
///
/// \returns the object if the cast succeeds, or null otherwise.
SWIFT_RUNTIME_EXPORT
const void *
swift_dynamicCastForeignClassUnconditional(
  const void *object,
  const ForeignClassMetadata *targetType,
  const char *file, unsigned line, unsigned column);
#endif

/// Checked dynamic cast of a class instance pointer to the given type.
///
/// \param object The class instance to cast.
///
/// \param targetType The type to which we are casting, which may be either a
/// class type or a wrapped Objective-C class type.
///
/// \returns the object, or null if it doesn't have the given target type.
SWIFT_RUNTIME_EXPORT
const void *
swift_dynamicCastUnknownClass(const void *object, const Metadata *targetType);

/// Unconditional checked dynamic cast of a class instance pointer to
/// the given type.
///
/// Aborts if the object isn't of the target type.
///
/// \param object The class instance to cast.
///
/// \param targetType The type to which we are casting, which may be either a
/// class type or a wrapped Objective-C class type.
///
/// \param file The source filename from which to report failure. May be null.
/// \param line The source line from which to report failure.
/// \param column The source column from which to report failure.
///
/// \returns the object.
SWIFT_RUNTIME_EXPORT
const void *
swift_dynamicCastUnknownClassUnconditional(const void *object,
                                           const Metadata *targetType,
                                           const char *file, unsigned line, unsigned column);

SWIFT_RUNTIME_EXPORT
const Metadata *
swift_dynamicCastMetatype(const Metadata *sourceType,
                          const Metadata *targetType);
SWIFT_RUNTIME_EXPORT
const Metadata *
swift_dynamicCastMetatypeUnconditional(const Metadata *sourceType,
                                       const Metadata *targetType,
                                       const char *file, unsigned line, unsigned column);
#if SWIFT_OBJC_INTEROP
SWIFT_RUNTIME_EXPORT
const ClassMetadata *
swift_dynamicCastObjCClassMetatype(const ClassMetadata *sourceType,
                                   const ClassMetadata *targetType);
SWIFT_RUNTIME_EXPORT
const ClassMetadata *
swift_dynamicCastObjCClassMetatypeUnconditional(const ClassMetadata *sourceType,
                                                const ClassMetadata *targetType,
                                                const char *file, unsigned line, unsigned column);
#endif

SWIFT_RUNTIME_EXPORT
const ClassMetadata *
swift_dynamicCastForeignClassMetatype(const ClassMetadata *sourceType,
                                   const ClassMetadata *targetType);
SWIFT_RUNTIME_EXPORT
const ClassMetadata *
swift_dynamicCastForeignClassMetatypeUnconditional(
  const ClassMetadata *sourceType,
  const ClassMetadata *targetType,
  const char *file, unsigned line, unsigned column);

/// Return the dynamic type of an opaque value.
///
/// \param value An opaque value.
/// \param self  The static type metadata for the opaque value and the result
///              type value.
/// \param existentialMetatype Whether the result type value is an existential
///                            metatype. If `self` is an existential type,
///                            then a `false` value indicates that the result
///                            is of concrete metatype type `self.Protocol`,
///                            and existential containers will not be projected
///                            through. A `true` value indicates that the result
///                            is of existential metatype type `self.Type`,
///                            so existential containers can be projected
///                            through as long as a subtype relationship holds
///                            from `self` to the contained dynamic type.
SWIFT_RUNTIME_EXPORT
const Metadata *
swift_getDynamicType(OpaqueValue *value, const Metadata *self,
                     bool existentialMetatype);

/// Fetch the type metadata associated with the formal dynamic
/// type of the given (possibly Objective-C) object.  The formal
/// dynamic type ignores dynamic subclasses such as those introduced
/// by KVO. See [NOTE: Dynamic-subclass-KVO]
///
/// The object pointer may be a tagged pointer, but cannot be null.
SWIFT_RUNTIME_EXPORT
const Metadata *swift_getObjectType(HeapObject *object);

/// Check whether a type conforms to a given native Swift protocol,
/// visible from the named module.
///
/// If so, returns a pointer to the witness table for its conformance.
/// Returns void if the type does not conform to the protocol.
///
/// \param type The metadata for the type for which to do the conformance
///             check.
/// \param protocol The protocol descriptor for the protocol to check
///                 conformance for. This pointer does not have ptrauth applied.
SWIFT_RUNTIME_EXPORT
const WitnessTable *swift_conformsToProtocol(const Metadata *type,
                                             const void *protocol);

/// Check whether a type conforms to a given native Swift protocol. Identical to
/// swift_conformsToProtocol, except that the protocol parameter has a ptrauth
/// signature on ARM64e that is signed with a process independent key.
SWIFT_RUNTIME_EXPORT
const WitnessTable *
swift_conformsToProtocol2(const Metadata *type,
                          const ProtocolDescriptor *protocol);

/// Check whether a type conforms to a given native Swift protocol. Identical to
/// swift_conformsToProtocol, except that the protocol parameter has a ptrauth
/// signature on ARM64e that is signed with a process dependent key.
SWIFT_RUNTIME_EXPORT
const WitnessTable *
swift_conformsToProtocolCommon(const Metadata *type,
                               const ProtocolDescriptor *protocol);
} // end namespace swift

#endif // SWIFT_RUNTIME_CASTING_H

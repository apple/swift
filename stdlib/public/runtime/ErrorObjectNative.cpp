//===--- ErrorObjectNative.cpp - Recoverable error object -----------------===//
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
// implementation is used when ObjC interop is disabled; the ObjC-interoperable
// version is implemented in ErrorObject.mm.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/Config.h"

#if !SWIFT_OBJC_INTEROP

#include <stdio.h>
#include "swift/Runtime/Debug.h"
#include "ErrorObject.h"
#include "Private.h"

using namespace swift;

/// Determine the size and alignment of an Error box containing the given
/// type.
static std::pair<size_t, size_t>
_getErrorAllocatedSizeAndAlignmentMask(const Metadata *type) {
  // The value is tail-allocated after the SwiftError record with the
  // appropriate alignment.
  auto vw = type->getValueWitnesses();
  size_t size = sizeof(SwiftError);
  unsigned valueAlignMask = vw->getAlignmentMask();
  size = (size + valueAlignMask) & ~(size_t)valueAlignMask;
  size += vw->getSize();
  
  size_t alignMask = (alignof(SwiftError) - 1) | valueAlignMask;
  
  return {size, alignMask};
}

/// Destructor for an Error box.
static SWIFT_CC void _destroyErrorObject(SWIFT_CONTEXT HeapObject *obj) {
  auto error = static_cast<SwiftError *>(obj);
  
  // Destroy the value inside.
  auto type = error->type;
  type->vw_destroy(error->getValue());
  
  // Deallocate the buffer.
  auto sizeAndAlign = _getErrorAllocatedSizeAndAlignmentMask(type);
  swift_deallocObject(obj, sizeAndAlign.first, sizeAndAlign.second);
}

/// Heap metadata for Error boxes.
static const FullMetadata<HeapMetadata> ErrorMetadata{
  HeapMetadataHeader{{_destroyErrorObject}, {&VALUE_WITNESS_SYM(Bo)}},
  HeapMetadata(MetadataKind::ErrorObject),
};

BoxPair
swift::swift_allocError(const swift::Metadata *type,
                        const swift::WitnessTable *errorConformance,
                        OpaqueValue *initialValue,
                        bool isTake) {
  auto sizeAndAlign = _getErrorAllocatedSizeAndAlignmentMask(type);
  
  auto allocated = swift_allocObject(&ErrorMetadata,
                                     sizeAndAlign.first, sizeAndAlign.second);
  
  auto error = reinterpret_cast<SwiftError*>(allocated);
  
  error->type = type;
  error->errorConformance = errorConformance;
  
  // If an initial value was given, copy or take it in.
  auto valuePtr = error->getValue();
  if (initialValue) {
    if (isTake)
      type->vw_initializeWithTake(valuePtr, initialValue);
    else
      type->vw_initializeWithCopy(valuePtr, initialValue);
  }
  
  return BoxPair{allocated, valuePtr};
}

void
swift::swift_deallocError(SwiftError *error, const Metadata *type) {
  auto sizeAndAlign = _getErrorAllocatedSizeAndAlignmentMask(type);
  swift_deallocUninitializedObject(error, sizeAndAlign.first, sizeAndAlign.second);
}

void
swift::swift_getErrorValue(const SwiftError *errorObject,
                           void **scratch,
                           ErrorValueResult *out) {
  out->value = errorObject->getValue();
  out->type = errorObject->type;
  out->errorConformance = errorObject->errorConformance;
}

#endif

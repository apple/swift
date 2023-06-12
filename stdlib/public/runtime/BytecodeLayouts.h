//===--- RuntimeValueWitness.h                                         ---===//
// Swift Language Bytecode Layouts Runtime Implementation
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
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BYTECODE_LAYOUTS_H
#define SWIFT_BYTECODE_LAYOUTS_H

#include "swift/Runtime/Metadata.h"
#include <cstdint>
#include <vector>

namespace swift {

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

  Metatype = 0x0c,
  Generic = 0x0d,
  Existential = 0x0e,
  Resilient = 0x0f,

  SinglePayloadEnumSimple = 0x10,
  SinglePayloadEnumFN = 0x11,
  SinglePayloadEnumFNResolved = 0x12,

  MultiPayloadEnumFN = 0x13,
  MultiPayloadEnumFNResolved = 0x14,

  Skip = 0x80,
  // We may use the MSB as flag that a count follows,
  // so all following values are reserved
  // Reserved: 0x81 - 0xFF
};

SWIFT_RUNTIME_EXPORT
void swift_generic_destroy(swift::OpaqueValue *address, const Metadata *metadata);
SWIFT_RUNTIME_EXPORT
swift::OpaqueValue *swift_generic_assignWithCopy(swift::OpaqueValue *dest, swift::OpaqueValue *src, const Metadata *metadata);
SWIFT_RUNTIME_EXPORT
swift::OpaqueValue *swift_generic_assignWithTake(swift::OpaqueValue *dest, swift::OpaqueValue *src, const Metadata *metadata);
SWIFT_RUNTIME_EXPORT
swift::OpaqueValue *swift_generic_initWithCopy(swift::OpaqueValue *dest, swift::OpaqueValue *src, const Metadata *metadata);
SWIFT_RUNTIME_EXPORT
swift::OpaqueValue *swift_generic_initWithTake(swift::OpaqueValue *dest, swift::OpaqueValue *src, const Metadata *metadata);
SWIFT_RUNTIME_EXPORT
void swift_generic_instantiateLayoutString(const uint8_t *layoutStr, Metadata *type);

void swift_resolve_resilientAccessors(uint8_t *layoutStr,
                                      size_t layoutStrOffset,
                                      const uint8_t *fieldLayoutStr,
                                      const Metadata *fieldType);
} // namespace swift

#endif // SWIFT_BYTECODE_LAYOUTS_H

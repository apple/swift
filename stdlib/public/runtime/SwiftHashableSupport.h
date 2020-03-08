//===------------------------------------------------------------*- C++ -*-===//
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

#ifndef SWIFT_RUNTIME_SWIFT_HASHABLE_SUPPORT_H
#define SWIFT_RUNTIME_SWIFT_HASHABLE_SUPPORT_H

#include "swift/Runtime/Metadata.h"
#include <stdint.h>

namespace swift {
namespace hashable_support {

extern "C" const ProtocolDescriptor PROTOCOL_DESCR_SYM(SH);
static constexpr auto &HashableProtocolDescriptor = PROTOCOL_DESCR_SYM(SH);

struct HashableWitnessTable;

/// Calls `Equatable.==` through a `Hashable` (not Equatable!) witness
/// table.
SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
bool _swift_stdlib_Hashable_isEqual_indirect(
    const void *lhsValue, const void *rhsValue, const Metadata *type,
    const HashableWitnessTable *wt);

/// Calls `Hashable.hashValue.get` through a `Hashable` witness table.
SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
intptr_t _swift_stdlib_Hashable_hashValue_indirect(
    const void *value, const Metadata *type, const HashableWitnessTable *wt);

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
void _swift_convertToAnyHashableIndirect(
    OpaqueValue *source, OpaqueValue *destination, const Metadata *sourceType,
    const HashableWitnessTable *sourceConformance);

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
bool _swift_anyHashableDownCastConditionalIndirect(
    OpaqueValue *source, OpaqueValue *destination, const Metadata *targetType);

/// Find the base type that introduces the `Hashable` conformance.
/// Because the provided type is known to conform to `Hashable`, this
/// function always returns non-null.
///
/// - Precondition: `type` conforms to `Hashable` (not checked).
const Metadata *findHashableBaseTypeOfHashableType(
    const Metadata *type);

/// Find the base type that introduces the `Hashable` conformance.
/// If `type` does not conform to `Hashable`, `nullptr` is returned.
const Metadata *findHashableBaseType(const Metadata *type);

} // namespace hashable_support
} // namespace swift

#endif


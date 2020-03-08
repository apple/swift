//===----------------------------------------------------------------------===//
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

#include "swift/Runtime/Metadata.h"
#include "swift/Demangling/ManglingMacros.h"

using namespace swift;

// Placeholders for Swift functions that the C++ runtime references
// but that the test code does not link to.

// AnyHashable

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
void _swift_makeAnyHashableUsingDefaultRepresentation(
  const OpaqueValue *value,
  const void *anyHashableResultPointer,
  const Metadata *T,
  const WitnessTable *hashableWT
) {
  abort();
}

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ss11AnyHashableVMn[1] = {0};

// SwiftHashableSupport

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
bool _swift_stdlib_Hashable_isEqual_indirect(
  const void *lhsValue, const void *rhsValue, const Metadata *type,
  const void *wt) {
  abort();
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
intptr_t _swift_stdlib_Hashable_hashValue_indirect(
  const void *value, const Metadata *type, const void *wt) {
  abort();
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
void _swift_convertToAnyHashableIndirect(
  OpaqueValue *source, OpaqueValue *destination, const Metadata *sourceType,
  const void *sourceConformance) {
  abort();
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
bool _swift_anyHashableDownCastConditionalIndirect(
  OpaqueValue *source, OpaqueValue *destination, const Metadata *targetType) {
  abort();
}

// Casting

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
void _swift_arrayDownCastIndirect(OpaqueValue *destination,
                                  OpaqueValue *source,
                                  const Metadata *sourceValueType,
                                  const Metadata *targetValueType) {
  abort();
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
bool _swift_arrayDownCastConditionalIndirect(OpaqueValue *destination,
                                             OpaqueValue *source,
                                             const Metadata *sourceValueType,
                                             const Metadata *targetValueType) {
  abort();
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
void _swift_setDownCastIndirect(OpaqueValue *destination,
                                OpaqueValue *source,
                                const Metadata *sourceValueType,
                                const Metadata *targetValueType,
                                const void *sourceValueHashable,
                                const void *targetValueHashable) {
  abort();
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
bool _swift_setDownCastConditionalIndirect(OpaqueValue *destination,
                                           OpaqueValue *source,
                                           const Metadata *sourceValueType,
                                           const Metadata *targetValueType,
                                           const void *sourceValueHashable,
                                           const void *targetValueHashable) {
  abort();
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
void _swift_dictionaryDownCastIndirect(OpaqueValue *destination,
                                       OpaqueValue *source,
                                       const Metadata *sourceKeyType,
                                       const Metadata *sourceValueType,
                                       const Metadata *targetKeyType,
                                       const Metadata *targetValueType,
                                       const void *sourceKeyHashable,
                                       const void *targetKeyHashable) {
  abort();
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
bool _swift_dictionaryDownCastConditionalIndirect(OpaqueValue *destination,
                                        OpaqueValue *source,
                                        const Metadata *sourceKeyType,
                                        const Metadata *sourceValueType,
                                        const Metadata *targetKeyType,
                                        const Metadata *targetValueType,
                                        const void *sourceKeyHashable,
                                        const void *targetKeyHashable) {
  abort();
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
void _bridgeNonVerbatimBoxedValue(const OpaqueValue *sourceValue,
                                  OpaqueValue *destValue,
                                  const Metadata *nativeType) {
  abort();
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
void _bridgeNonVerbatimFromObjectiveCToAny(HeapObject *sourceValue,
                                           OpaqueValue *destValue) {
  abort();
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
bool swift_unboxFromSwiftValueWithType(OpaqueValue *source,
                                       OpaqueValue *result,
                                       const Metadata *destinationType) {
  abort();
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
bool swift_swiftValueConformsTo(const Metadata *destinationType) {
  abort();
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_API
HeapObject *$ss27_bridgeAnythingToObjectiveCyyXlxlF(OpaqueValue *src, const Metadata *srcType) {
  abort();
}

// ErrorObject

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
int $ss13_getErrorCodeySiSPyxGs0B0RzlF(void *) {
  abort();
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
void *$ss23_getErrorDomainNSStringyyXlSPyxGs0B0RzlF(void *) {
  abort();
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
void *$ss29_getErrorUserInfoNSDictionaryyyXlSgSPyxGs0B0RzlF(void *) {
  abort();
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
void *$ss32_getErrorEmbeddedNSErrorIndirectyyXlSgSPyxGs0B0RzlF(void *) {
  abort();
}

// Hashable

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $SkMp[1] = {0};

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSHMp[1] = {0};

// Array

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSaMn[1] = {0};

// Dictionary

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ssSdVMn[1] = {0};

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSDMn[1] = {0};

// Set

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ssSeVMn[1] = {0};

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sShMn[1] = {0};

// Bool

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSbMn[1] = {0};

// Binary Floating Point

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSBMp[1] = {0};

// Double

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSdMn[1] = {0};

// RandomNumberGenerator

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSGMp[1] = {0};

// Int

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSiMn[1] = {0};

// DefaultIndicis

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSIMn[1] = {0};

// Character

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSJMn[1] = {0};

// Numeric

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSjMp[1] = {0};

// RandomAccessCollection

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSkMp[1] = {0};

// BidirectionalCollection

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSKMp[1] = {0};

// RangeReplacementCollection

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSmMp[1] = {0};

// MutationCollection

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSMMp[1] = {0};

// Range

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSnMn[1] = {0};

// ClosedRange

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSNMn[1] = {0};

// ObjectIdentifier

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSOMn[1] = {0};

// UnsafeMutablePointer

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSpMn[1] = {0};

// Optional

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSqMn[1] = {0};

// Equatable

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSQMp[1] = {0};

// UnsafeMutableBufferPointer

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSrMn[1] = {0};

// UnsafeBufferPointer

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSRMn[1] = {0};

// String

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSSMn[1] = {0};

// Sequence

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSTMp[1] = {0};

// UInt

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSuMn[1] = {0};

// UnsignedInteger

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSUMp[1] = {0};

// UnsafeMutableRawPointer

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSvMn[1] = {0};

// Strideable

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSxMp[1] = {0};

// RangeExpression

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSXMp[1] = {0};

// StringProtocol

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSyMp[1] = {0};

// RawRepresentable

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSYMp[1] = {0};

// BinaryInteger

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSzMp[1] = {0};

// Decodable

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSeMp[1] = {0};

// Encodable

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSEMp[1] = {0};

// Float

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSfMn[1] = {0};

// FloatingPoint

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSFMp[1] = {0};

// Collection

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSlMp[1] = {0};

// Comparable

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSLMp[1] = {0};

// UnsafePointer

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSPMn[1] = {0};

// Substring

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSsMn[1] = {0};

// IteratorProtocol

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sStMp[1] = {0};

// UnsafeRawPointer

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSVMn[1] = {0};

// UnsafeMutableRawBufferPointer

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSwMn[1] = {0};

// UnsafeRawBufferPointer

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSWMn[1] = {0};

// SignedInteger

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $sSZMp[1] = {0};

// Mirror

// protocol witness table for Swift._ClassSuperMirror : Swift._Mirror in Swift
SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ss17_ClassSuperMirrorVs01_C0sWP[1] = {0};

// type metadata accessor for Swift._ClassSuperMirror
SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ss17_ClassSuperMirrorVMa[1] = {0};

// protocol witness table for Swift._MetatypeMirror : Swift._Mirror in Swift
SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ss15_MetatypeMirrorVs01_B0sWP[1] = {0};

// type metadata accessor for Swift._MetatypeMirror
SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ss15_MetatypeMirrorVMa[1] = {0};

// protocol witness table for Swift._EnumMirror : Swift._Mirror in Swift
SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ss11_EnumMirrorVs01_B0sWP[1] = {0};

// type metadata accessor for Swift._EnumMirror
SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ss11_EnumMirrorVMa[1] = {0};

// protocol witness table for Swift._OpaqueMirror : Swift._Mirror in Swift
SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ss13_OpaqueMirrorVs01_B0sWP[1] = {0};

// type metadata accessor for Swift._OpaqueMirror
SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ss13_OpaqueMirrorVMa[1] = {0};

// protocol witness table for Swift._StructMirror : Swift._Mirror in Swift
SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ss13_StructMirrorVs01_B0sWP[1] = {0};

// type metadata accessor for Swift._StructMirror
SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ss13_StructMirrorVMa[1] = {0};

// protocol witness table for Swift._TupleMirror : Swift._Mirror in Swift
SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ss12_TupleMirrorVs01_B0sWP[1] = {0};

// type metadata accessor for Swift._TupleMirror
SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ss12_TupleMirrorVMa[1] = {0};

// protocol witness table for Swift._ClassMirror : Swift._Mirror in Swift
SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ss12_ClassMirrorVs01_B0sWP[1] = {0};

// type metadata accessor for Swift._ClassMirror
SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ss12_ClassMirrorVMa[1] = {0};

// KeyPath

SWIFT_RUNTIME_STDLIB_SPI
const HeapObject *swift_getKeyPathImpl(const void *p,
                                       const void *e,
                                       const void *a) {
  abort();
}

// playground print hook

struct swift_closure {
  void *fptr;
  HeapObject *context;
};
SWIFT_RUNTIME_STDLIB_API SWIFT_CC swift_closure
MANGLE_SYM(s20_playgroundPrintHookySScSgvg)() {
  return {nullptr, nullptr};
}

// ObjectiveC Bridgeable

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ss21_ObjectiveCBridgeableMp[1] = {0};

// Key Path

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ss7KeyPathCMo[1] = {0};

// Boxing

SWIFT_RUNTIME_STDLIB_INTERNAL
const long long $ss12__SwiftValueCMn[1] = {0};

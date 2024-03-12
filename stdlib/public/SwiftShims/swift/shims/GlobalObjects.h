//===--- GlobalObjects.h - Statically-initialized objects -------*- C++ -*-===//
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
//  Objects that are allocated at global scope instead of on the heap,
//  and statically initialized to avoid synchronization costs, are
//  defined here.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_STDLIB_SHIMS_GLOBALOBJECTS_H_
#define SWIFT_STDLIB_SHIMS_GLOBALOBJECTS_H_

#include "SwiftStdint.h"
#include "SwiftStdbool.h"
#include "HeapObject.h"
#include "Visibility.h"

#ifdef __cplusplus
#ifndef __swift__
namespace swift {
#endif
extern "C" {
#endif

struct _SwiftArrayBodyStorage {
  __swift_intptr_t count;
  __swift_uintptr_t _capacityAndFlags;
};

struct _SwiftEmptyArrayStorage {
  struct HeapObject header;
  struct _SwiftArrayBodyStorage body;
};

SWIFT_RUNTIME_STDLIB_API SWIFT_CONSTANT_RELOCATABLE_DATA
const struct _SwiftEmptyArrayStorage _swiftEmptyArrayStorage;

static inline const void * _Nonnull _swift_stdlib_getEmptyArrayStorage() {
  return &_swiftEmptyArrayStorage;
}

struct _SwiftDictionaryBodyStorage {
  __swift_intptr_t count;
  __swift_intptr_t capacity;
  __swift_int8_t scale;
  __swift_int8_t reservedScale;
  __swift_int16_t extra;
  __swift_int32_t age;
  __swift_intptr_t seed;
  __swift_uintptr_t rawKeys;
  __swift_uintptr_t rawValues;
};

struct _SwiftSetBodyStorage {
  __swift_intptr_t count;
  __swift_intptr_t capacity;
  __swift_int8_t scale;
  __swift_int8_t reservedScale;
  __swift_int16_t extra;
  __swift_int32_t age;
  __swift_intptr_t seed;
  __swift_uintptr_t rawElements;
};

struct _SwiftEmptyDictionarySingleton {
  struct HeapObject header;
  struct _SwiftDictionaryBodyStorage body;
  __swift_uintptr_t metadata;
};

struct _SwiftEmptySetSingleton {
  struct HeapObject header;
  struct _SwiftSetBodyStorage body;
  __swift_uintptr_t metadata;
};

SWIFT_RUNTIME_STDLIB_API SWIFT_CONSTANT_RELOCATABLE_DATA
const struct _SwiftEmptyDictionarySingleton _swiftEmptyDictionarySingleton;

static inline const void * _Nonnull _swift_stdlib_getEmptyDictionarySingleton() {
  return &_swiftEmptyDictionarySingleton;
}

SWIFT_RUNTIME_STDLIB_API SWIFT_CONSTANT_RELOCATABLE_DATA
const struct _SwiftEmptySetSingleton _swiftEmptySetSingleton;

static inline const void * _Nonnull _swift_stdlib_getEmptySetSingleton() {
  return &_swiftEmptySetSingleton;
}

struct _SwiftHashingParameters {
  __swift_uint64_t seed0;
  __swift_uint64_t seed1;
  __swift_bool deterministic;
};
  
SWIFT_RUNTIME_STDLIB_API
const struct _SwiftHashingParameters _swift_stdlib_Hashing_parameters;

#ifdef __cplusplus

static_assert(
  sizeof(_SwiftDictionaryBodyStorage) ==
    5 * sizeof(__swift_intptr_t) + sizeof(__swift_int64_t),
  "_SwiftDictionaryBodyStorage has unexpected size");

static_assert(
  sizeof(_SwiftSetBodyStorage) ==
    4 * sizeof(__swift_intptr_t) + sizeof(__swift_int64_t),
  "_SwiftSetBodyStorage has unexpected size");

} // extern "C"
#ifndef __swift__
} // namespace swift
#endif
#endif

#endif

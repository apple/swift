//===--- Enum.cpp - Enum tests --------------------------------------------===//
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
#include "swift/Runtime/Enum.h"
#include "gtest/gtest.h"

using namespace swift;

SWIFT_CC
static unsigned byte_getExtraInhabitantTag(const OpaqueValue *src,
                                           unsigned numXI,
                                           const Metadata *self) {
  assert(numXI == 2);
  uint8_t byte = *reinterpret_cast<const uint8_t*>(src);
  if (byte > 253)
    return byte - 253;
  return 0;
}

static unsigned byte_getEnumTagSinglePayload(const OpaqueValue *value,
                                             unsigned numEmptyCases,
                                             const Metadata *self) {
  return swift_getEnumTagSinglePayloadGeneric(value, numEmptyCases, self,
                                              byte_getExtraInhabitantTag);
}

SWIFT_CC
static void byte_storeExtraInhabitantTag(OpaqueValue *dest, unsigned tag,
                                         unsigned numXI, const Metadata *self) {
  assert(numXI == 2);
  assert(tag > 0 && tag <= 2);
  *reinterpret_cast<uint8_t*>(dest) = 253 + tag;
}

static void byte_storeEnumTagSinglePayload(OpaqueValue *value,
                                           unsigned tag,
                                           unsigned numEmptyCases,
                                           const Metadata *self) {
  swift_storeEnumTagSinglePayloadGeneric(value, tag, numEmptyCases, self,
                                         byte_storeExtraInhabitantTag);
}

// Just use the normal operations for copying i8.
#define byte_initializeBufferWithCopyOfBuffer \
  VALUE_WITNESS_SYM(Bi8_).initializeBufferWithCopyOfBuffer
#define byte_destroy            VALUE_WITNESS_SYM(Bi8_).destroy
#define byte_initializeWithCopy VALUE_WITNESS_SYM(Bi8_).initializeWithCopy
#define byte_assignWithCopy     VALUE_WITNESS_SYM(Bi8_).assignWithCopy
#define byte_initializeWithTake VALUE_WITNESS_SYM(Bi8_).initializeWithTake
#define byte_assignWithTake     VALUE_WITNESS_SYM(Bi8_).assignWithTake

// Mock up a value witness table for Builtin.Int8 will 254 and 255 as extra
// inhabitants.
ValueWitnessTable Int8WithExtraInhabitantValueWitness = {
#define WANT_ONLY_REQUIRED_VALUE_WITNESSES
#define VALUE_WITNESS(LOWER_ID, UPPER_ID) byte_##LOWER_ID,
#define DATA_VALUE_WITNESS(LOWER_ID, UPPER_ID, TYPE)
#include "swift/ABI/ValueWitness.def"
  VALUE_WITNESS_SYM(Bi8_).size,
  VALUE_WITNESS_SYM(Bi8_).stride,
  VALUE_WITNESS_SYM(Bi8_).flags,
  /*extraInhabitantCount*/ 2
};

FullMetadata<OpaqueMetadata> XI_TMBi8_ = {
  {&Int8WithExtraInhabitantValueWitness},
  {{MetadataKind::Opaque}}
};

const OpaqueValue *asOpaque(const void *v) {
  return reinterpret_cast<const OpaqueValue*>(v);
}
OpaqueValue *asOpaque(void *v) {
  return reinterpret_cast<OpaqueValue*>(v);
}

int test_getEnumCaseSinglePayload(std::initializer_list<uint8_t> repr,
                                  const FullOpaqueMetadata &metadata,
                                  unsigned numEmptyCases) {
  return metadata.base.vw_getEnumTagSinglePayload(asOpaque(repr.begin()),
                                                  numEmptyCases);
}

TEST(EnumTest, getEnumCaseSinglePayload) {
  // Test with no XI.
  ASSERT_EQ(0, test_getEnumCaseSinglePayload({0, 0}, METADATA_SYM(Bi8_), 512));
  ASSERT_EQ(0, test_getEnumCaseSinglePayload({255, 0}, METADATA_SYM(Bi8_), 512));

  ASSERT_EQ(1, test_getEnumCaseSinglePayload({0, 1}, METADATA_SYM(Bi8_), 512));
  ASSERT_EQ(256, test_getEnumCaseSinglePayload({255, 1}, METADATA_SYM(Bi8_), 512));
  ASSERT_EQ(512, test_getEnumCaseSinglePayload({255, 2}, METADATA_SYM(Bi8_), 512));

  ASSERT_EQ(0, test_getEnumCaseSinglePayload({0, 0, 0}, METADATA_SYM(Bi8_),
                                               128*1024));
  ASSERT_EQ(0, test_getEnumCaseSinglePayload({255, 0, 0}, METADATA_SYM(Bi8_),
                                               128*1024));
#if defined(__BIG_ENDIAN__)
  ASSERT_EQ(65535 - 255 + 1, test_getEnumCaseSinglePayload(
                                 {0, 1, 0}, METADATA_SYM(Bi8_), 128 * 1024));
#else
  ASSERT_EQ(65535 - 255 + 1, test_getEnumCaseSinglePayload(
                                 {0, 0, 1}, METADATA_SYM(Bi8_), 128 * 1024));
#endif

  // Test with XI.
  ASSERT_EQ(0, test_getEnumCaseSinglePayload({0}, XI_TMBi8_, 2));
  ASSERT_EQ(0, test_getEnumCaseSinglePayload({253}, XI_TMBi8_, 2));
  ASSERT_EQ(1, test_getEnumCaseSinglePayload({254}, XI_TMBi8_, 2));
  ASSERT_EQ(2, test_getEnumCaseSinglePayload({255}, XI_TMBi8_, 2));

  ASSERT_EQ(0, test_getEnumCaseSinglePayload({0, 0}, XI_TMBi8_, 4));
  ASSERT_EQ(0, test_getEnumCaseSinglePayload({253, 0}, XI_TMBi8_, 4));
  ASSERT_EQ(1, test_getEnumCaseSinglePayload({254, 0}, XI_TMBi8_, 4));
  ASSERT_EQ(2, test_getEnumCaseSinglePayload({255, 0}, XI_TMBi8_, 4));
  ASSERT_EQ(3, test_getEnumCaseSinglePayload({0, 1}, XI_TMBi8_, 4));
  ASSERT_EQ(4, test_getEnumCaseSinglePayload({1, 1}, XI_TMBi8_, 4));
}

bool test_storeEnumTagSinglePayload(std::initializer_list<uint8_t> after,
                                     std::initializer_list<uint8_t> before,
                                     const FullOpaqueMetadata &metadata,
                                     unsigned whichCase,
                                     unsigned numEmptyCases) {
  assert(after.size() == before.size());

  std::vector<uint8_t> buf;
  buf.resize(before.size());
  memcpy(buf.data(), before.begin(), before.size());

  metadata.base.vw_storeEnumTagSinglePayload(asOpaque(buf.data()),
                                             whichCase,
                                             numEmptyCases);

  return memcmp(buf.data(), after.begin(), after.size()) == 0;
}

TEST(EnumTest, storeEnumTagSinglePayload) {
  // Test with no XI.
  ASSERT_TRUE(test_storeEnumTagSinglePayload({219, 0}, {219, 123},
                                              METADATA_SYM(Bi8_), 0, 512));
  ASSERT_TRUE(test_storeEnumTagSinglePayload({0, 1}, {219, 123},
                                              METADATA_SYM(Bi8_), 1, 512));
  ASSERT_TRUE(test_storeEnumTagSinglePayload({255, 1}, {219, 123},
                                              METADATA_SYM(Bi8_), 256, 512));
  ASSERT_TRUE(test_storeEnumTagSinglePayload({255, 2}, {219, 123},
                                              METADATA_SYM(Bi8_), 512, 512));

  ASSERT_TRUE(test_storeEnumTagSinglePayload({219, 0, 0}, {219, 123, 77},
                                              METADATA_SYM(Bi8_), 0, 128*1024));
#if defined(__BIG_ENDIAN__)
  ASSERT_TRUE(test_storeEnumTagSinglePayload({0, 0, 1}, {219, 123, 77},
                                              METADATA_SYM(Bi8_), 1, 128*1024));
  ASSERT_TRUE(test_storeEnumTagSinglePayload({255, 0, 1}, {219, 123, 77},
                                              METADATA_SYM(Bi8_), 256, 128*1024));
  ASSERT_TRUE(test_storeEnumTagSinglePayload({0, 0, 2}, {219, 123, 77},
                                              METADATA_SYM(Bi8_), 257, 128*1024));
  ASSERT_TRUE(test_storeEnumTagSinglePayload({255, 2, 0}, {219, 123, 77},
                                              METADATA_SYM(Bi8_), 128*1024, 128*1024));
#else
  ASSERT_TRUE(test_storeEnumTagSinglePayload({0, 1, 0}, {219, 123, 77},
                                              METADATA_SYM(Bi8_), 1, 128*1024));
  ASSERT_TRUE(test_storeEnumTagSinglePayload({255, 1, 0}, {219, 124, 77},
                                              METADATA_SYM(Bi8_), 256, 128*1024));
  ASSERT_TRUE(test_storeEnumTagSinglePayload({0, 2, 0}, {219, 123, 77},
                                              METADATA_SYM(Bi8_), 257, 128*1024));
  ASSERT_TRUE(test_storeEnumTagSinglePayload(
      {255, 0, 2}, {219, 123, 77}, METADATA_SYM(Bi8_), 128 * 1024, 128 * 1024));
#endif

  // Test with XI.
  ASSERT_TRUE(test_storeEnumTagSinglePayload({219}, {219},
                                              XI_TMBi8_, 0, 2));
  ASSERT_TRUE(test_storeEnumTagSinglePayload({254}, {219},
                                              XI_TMBi8_, 1, 2));
  ASSERT_TRUE(test_storeEnumTagSinglePayload({255}, {219},
                                              XI_TMBi8_, 2, 2));
  ASSERT_TRUE(test_storeEnumTagSinglePayload({219, 0}, {219, 123},
                                              XI_TMBi8_, 0, 4));
  ASSERT_TRUE(test_storeEnumTagSinglePayload({254, 0}, {219, 123},
                                              XI_TMBi8_, 1, 4));
  ASSERT_TRUE(test_storeEnumTagSinglePayload({255, 0}, {219, 123},
                                              XI_TMBi8_, 2, 4));
  ASSERT_TRUE(test_storeEnumTagSinglePayload({0, 1}, {219, 123},
                                              XI_TMBi8_, 3, 4));
  ASSERT_TRUE(test_storeEnumTagSinglePayload({1, 1}, {219, 123},
                                              XI_TMBi8_, 4, 4));
}

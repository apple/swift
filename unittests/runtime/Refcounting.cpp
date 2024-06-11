//===--- Refcounting.cpp - Reference-counting for Swift -------------------===//
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
#include "gtest/gtest.h"

using namespace swift;

struct TestObject : HeapObject {
  constexpr TestObject(HeapMetadata const *newMetadata)
    : HeapObject(newMetadata, InlineRefCounts::Immortal)
    , Addr(NULL), Value(0) {}

  size_t *Addr;
  size_t Value;
};

static SWIFT_CC(swift) void destroyTestObject(SWIFT_CONTEXT HeapObject *_object) {
  auto object = static_cast<TestObject*>(_object);
  assert(object->Addr && "object already deallocated");
  *object->Addr += object->Value;
  object->Addr = nullptr;
  swift_deallocObject(object, sizeof(TestObject), alignof(TestObject) - 1);
}

static const FullMetadata<ClassMetadata> TestClassObjectMetadata = {
  { { nullptr }, { &destroyTestObject }, { &VALUE_WITNESS_SYM(Bo) } },
  { { nullptr }, ClassFlags::UsesSwiftRefcounting, 0, 0, 0, 0, 0, 0 }
};

static TestObject ImmortalTestObject{&TestClassObjectMetadata};

/// Create an object that, when deallocated, adds the given value to
/// the value at the given pointer.
static TestObject *allocTestObject(size_t *addr, size_t value) {
  auto result =
    static_cast<TestObject *>(swift_allocObject(&TestClassObjectMetadata,
                                                sizeof(TestObject),
                                                alignof(TestObject) - 1));
  result->Addr = addr;
  result->Value = value;
  return result;
}

TEST(RefcountingTest, release) {
  size_t value = 0;
  auto object = allocTestObject(&value, 1);
  EXPECT_EQ(0u, value);
  swift_release(object);
  EXPECT_EQ(1u, value);
}

TEST(RefcountingTest, retain_release) {
  size_t value = 0;
  auto object = allocTestObject(&value, 1);
  EXPECT_EQ(0u, value);
  swift_retain(object);
  EXPECT_EQ(0u, value);
  swift_release(object);
  EXPECT_EQ(0u, value);
  swift_release(object);
  EXPECT_EQ(1u, value);
}

TEST(RefcountingTest, retain_release_n) {
  size_t value = 0;
  auto object = allocTestObject(&value, 1);
  EXPECT_EQ(0u, value);
  swift_retain_n(object, 32);
  swift_retain(object);
  EXPECT_EQ(0u, value);
  EXPECT_EQ(34u, swift_retainCount(object));
  swift_release_n(object, 31);
  EXPECT_EQ(0u, value);
  EXPECT_EQ(3u, swift_retainCount(object));
  swift_release(object);
  EXPECT_EQ(0u, value);
  EXPECT_EQ(2u, swift_retainCount(object));
  swift_release_n(object, 1);
  EXPECT_EQ(0u, value);
  EXPECT_EQ(1u, swift_retainCount(object));
}

TEST(RefcountingTest, unknown_retain_release_n) {
  size_t value = 0;
  auto object = allocTestObject(&value, 1);
  EXPECT_EQ(0u, value);
  swift_unknownObjectRetain_n(object, 32);
  swift_unknownObjectRetain(object);
  EXPECT_EQ(0u, value);
  EXPECT_EQ(34u, swift_retainCount(object));
  swift_unknownObjectRelease_n(object, 31);
  EXPECT_EQ(0u, value);
  EXPECT_EQ(3u, swift_retainCount(object));
  swift_unknownObjectRelease(object);
  EXPECT_EQ(0u, value);
  EXPECT_EQ(2u, swift_retainCount(object));
  swift_unknownObjectRelease_n(object, 1);
  EXPECT_EQ(0u, value);
  EXPECT_EQ(1u, swift_retainCount(object));
}

TEST(RefcountingTest, unowned_retain_release_n) {
  size_t value = 0;
  auto object = allocTestObject(&value, 1);
  EXPECT_EQ(0u, value);
  swift_unownedRetain_n(object, 32);
  swift_unownedRetain(object);
  EXPECT_EQ(34u, swift_unownedRetainCount(object));
  swift_unownedRelease_n(object, 31);
  EXPECT_EQ(3u, swift_unownedRetainCount(object));
  swift_unownedRelease(object);
  EXPECT_EQ(2u, swift_unownedRetainCount(object));
  swift_unownedRelease_n(object, 1);
  EXPECT_EQ(1u, swift_unownedRetainCount(object));
  swift_release(object);
  EXPECT_EQ(1u, value);
}

TEST(RefcountingTest, unowned_retain_release_n_overflow) {
  // This test would test overflow on 32bit platforms.
  // These platforms have 7 unowned reference count bits.
  size_t value = 0;
  auto object = allocTestObject(&value, 1);
  EXPECT_EQ(0u, value);
  swift_unownedRetain_n(object, 128);
  EXPECT_EQ(129u, swift_unownedRetainCount(object));
  swift_unownedRetain(object);
  EXPECT_EQ(130u, swift_unownedRetainCount(object));
  swift_unownedRelease_n(object, 128);
  EXPECT_EQ(2u, swift_unownedRetainCount(object));
  swift_unownedRelease(object);
  EXPECT_EQ(1u, swift_unownedRetainCount(object));
  swift_release(object);
  EXPECT_EQ(1u, value);
}

TEST(RefcountingTest, isUniquelyReferenced) {
  size_t value = 0;
  auto object = allocTestObject(&value, 1);
  EXPECT_EQ(0u, value);
  EXPECT_TRUE(swift_isUniquelyReferenced_nonNull_native(object));

  swift_retain(object);
  EXPECT_FALSE(swift_isUniquelyReferenced_nonNull_native(object));

  swift_release(object);
  EXPECT_TRUE(swift_isUniquelyReferenced_nonNull_native(object));

  swift_release(object);
  EXPECT_EQ(1u, value);
}

/////////////////////////////////////////
// Non-atomic reference counting tests //
/////////////////////////////////////////

TEST(RefcountingTest, nonatomic_release) {
  size_t value = 0;
  auto object = allocTestObject(&value, 1);
  EXPECT_EQ(0u, value);
  swift_nonatomic_release(object);
  EXPECT_EQ(1u, value);
}

TEST(RefcountingTest, nonatomic_retain_release) {
  size_t value = 0;
  auto object = allocTestObject(&value, 1);
  EXPECT_EQ(0u, value);
  swift_nonatomic_retain(object);
  EXPECT_EQ(0u, value);
  swift_nonatomic_release(object);
  EXPECT_EQ(0u, value);
  swift_nonatomic_release(object);
  EXPECT_EQ(1u, value);
}

TEST(RefcountingTest, nonatomic_retain_release_n) {
  size_t value = 0;
  auto object = allocTestObject(&value, 1);
  EXPECT_EQ(0u, value);
  auto res = swift_nonatomic_retain_n(object, 32);
  EXPECT_EQ(object, res);
  res = swift_nonatomic_retain(object);
  EXPECT_EQ(object, res);
  EXPECT_EQ(0u, value);
  EXPECT_EQ(34u, swift_retainCount(object));
  swift_nonatomic_release_n(object, 31);
  EXPECT_EQ(0u, value);
  EXPECT_EQ(3u, swift_retainCount(object));
  swift_nonatomic_release(object);
  EXPECT_EQ(0u, value);
  EXPECT_EQ(2u, swift_retainCount(object));
  swift_nonatomic_release_n(object, 1);
  EXPECT_EQ(0u, value);
  EXPECT_EQ(1u, swift_retainCount(object));
}

TEST(RefcountingTest, nonatomic_unknown_retain_release_n) {
  size_t value = 0;
  auto object = allocTestObject(&value, 1);
  EXPECT_EQ(0u, value);
  auto res = swift_nonatomic_unknownObjectRetain_n(object, 32);
  EXPECT_EQ(object, res);
  res = swift_nonatomic_unknownObjectRetain(object);
  EXPECT_EQ(object, res);
  EXPECT_EQ(0u, value);
  EXPECT_EQ(34u, swift_retainCount(object));
  swift_nonatomic_unknownObjectRelease_n(object, 31);
  EXPECT_EQ(0u, value);
  EXPECT_EQ(3u, swift_retainCount(object));
  swift_nonatomic_unknownObjectRelease(object);
  EXPECT_EQ(0u, value);
  EXPECT_EQ(2u, swift_retainCount(object));
  swift_nonatomic_unknownObjectRelease_n(object, 1);
  EXPECT_EQ(0u, value);
  EXPECT_EQ(1u, swift_retainCount(object));
}

// Verify that refcounting operations on immortal objects never changes the
// refcount field.
TEST(RefcountingTest, immortal_retain_release) {
  auto initialBitsValue = ImmortalTestObject.refCounts.getBitsValue();

  swift_retain(&ImmortalTestObject);
  EXPECT_EQ(initialBitsValue, ImmortalTestObject.refCounts.getBitsValue());
  swift_release(&ImmortalTestObject);
  EXPECT_EQ(initialBitsValue, ImmortalTestObject.refCounts.getBitsValue());
  swift_nonatomic_retain(&ImmortalTestObject);
  EXPECT_EQ(initialBitsValue, ImmortalTestObject.refCounts.getBitsValue());
  swift_nonatomic_release(&ImmortalTestObject);
  EXPECT_EQ(initialBitsValue, ImmortalTestObject.refCounts.getBitsValue());

  swift_unownedRetain(&ImmortalTestObject);
  EXPECT_EQ(initialBitsValue, ImmortalTestObject.refCounts.getBitsValue());
  swift_unownedRelease(&ImmortalTestObject);
  EXPECT_EQ(initialBitsValue, ImmortalTestObject.refCounts.getBitsValue());
  swift_nonatomic_unownedRetain(&ImmortalTestObject);
  EXPECT_EQ(initialBitsValue, ImmortalTestObject.refCounts.getBitsValue());
  swift_nonatomic_unownedRelease(&ImmortalTestObject);
  EXPECT_EQ(initialBitsValue, ImmortalTestObject.refCounts.getBitsValue());

  for (unsigned i = 0; i < 32; i++) {
    uint32_t amount = 1U << i;

    swift_retain_n(&ImmortalTestObject, amount);
    EXPECT_EQ(initialBitsValue, ImmortalTestObject.refCounts.getBitsValue());
    swift_release_n(&ImmortalTestObject, amount);
    EXPECT_EQ(initialBitsValue, ImmortalTestObject.refCounts.getBitsValue());
    swift_nonatomic_retain_n(&ImmortalTestObject, amount);
    EXPECT_EQ(initialBitsValue, ImmortalTestObject.refCounts.getBitsValue());
    swift_nonatomic_release_n(&ImmortalTestObject, amount);
    EXPECT_EQ(initialBitsValue, ImmortalTestObject.refCounts.getBitsValue());
    
    swift_unownedRetain_n(&ImmortalTestObject, amount);
    EXPECT_EQ(initialBitsValue, ImmortalTestObject.refCounts.getBitsValue());
    swift_unownedRelease_n(&ImmortalTestObject, amount);
    EXPECT_EQ(initialBitsValue, ImmortalTestObject.refCounts.getBitsValue());
    swift_nonatomic_unownedRetain_n(&ImmortalTestObject, amount);
    EXPECT_EQ(initialBitsValue, ImmortalTestObject.refCounts.getBitsValue());
    swift_nonatomic_unownedRelease_n(&ImmortalTestObject, amount);
    EXPECT_EQ(initialBitsValue, ImmortalTestObject.refCounts.getBitsValue());
  }
}

///////////////////////////////////////////
// Multi-object reference counting tests //
///////////////////////////////////////////

TEST(RefcountingTest, retain_release_multiple) {
  size_t value = 0;
  const size_t count = 16;
  HeapObject *objects[count];
  for (size_t n = 0; n < count; ++n) {
    objects[n] = allocTestObject(&value, 1 << n);
  }
  EXPECT_EQ(0u, value);
  swift_retainMultiple(objects, count);
  EXPECT_EQ(2u, swift_retainCount(objects[7]));
  EXPECT_EQ(0u, value);
  swift_retainMultiple(objects, count);
  EXPECT_EQ(3u, swift_retainCount(objects[13]));
  EXPECT_EQ(0u, value);
  swift_releaseMultiple(objects, count);
  EXPECT_EQ(2u, swift_retainCount(objects[4]));
  EXPECT_EQ(0u, value);
  swift_releaseMultiple(objects, count);
  EXPECT_EQ(1u, swift_retainCount(objects[15]));
  EXPECT_EQ(0u, value);
  swift_releaseMultiple(objects, count);
  EXPECT_EQ(0xffffu, value);
}

//===--- weak.mm - Weak-pointer tests -------------------------------------===//
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

#include <Foundation/NSObject.h>
#include <objc/runtime.h>
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Metadata.h"
#include "gtest/gtest.h"

using namespace swift;

// A fake definition of Swift runtime's WeakReference.
// This has the proper size and alignment which is all we need.
namespace swift {
class WeakReference { void *value __attribute__((unused)); };
}

// Declare some Objective-C stuff.
extern "C" void objc_release(id);

static unsigned DestroyedObjCCount = 0;

/// A trivial class that increments DestroyedObjCCount when deallocated.
@interface ObjCClass : NSObject @end
@implementation ObjCClass
- (void) dealloc {
  DestroyedObjCCount++;
  [super dealloc];
}
@end
static HeapObject *make_objc_object() {
  return (HeapObject*) [ObjCClass new];
}

// Make a Native Swift object by calling a Swift function.
// _swift_StdlibUnittest_make_swift_object is implemented in StdlibUnittest.
SWIFT_CC extern "C"
HeapObject *_swift_StdlibUnittest_make_swift_object();

static HeapObject *make_swift_object() {
  return _swift_StdlibUnittest_make_swift_object();
}

static unsigned getUnownedRetainCount(HeapObject *object) {
  return swift_unownedRetainCount(object) - 1;
}

static void unknown_release(void *value) {
  objc_release((id) value);
}

TEST(WeakTest, preconditions) {
  swift_release(make_swift_object());
  unknown_release(make_objc_object());
}

TEST(WeakTest, simple_swift) {
  HeapObject *o1 = make_swift_object();
  HeapObject *o2 = make_swift_object();
  ASSERT_NE(o1, o2);
  ASSERT_NE(o1, nullptr);
  ASSERT_NE(o2, nullptr);

  WeakReference ref1;
  auto res = swift_weakInit(&ref1, o1);
  ASSERT_EQ(res, &ref1);

  HeapObject *tmp = swift_weakLoadStrong(&ref1);
  ASSERT_EQ(tmp, o1);
  swift_release(tmp);

  tmp = swift_weakLoadStrong(&ref1);
  ASSERT_EQ(o1, tmp);
  swift_release(tmp);

  res = swift_weakAssign(&ref1, o2);
  ASSERT_EQ(res, &ref1);
  tmp = swift_weakLoadStrong(&ref1);
  ASSERT_EQ(o2, tmp);
  swift_release(tmp);

  tmp = swift_weakLoadStrong(&ref1);
  ASSERT_EQ(o2, tmp);
  swift_release(tmp);

  swift_release(o1);

  tmp = swift_weakLoadStrong(&ref1);
  ASSERT_EQ(o2, tmp);
  swift_release(tmp);

  swift_release(o2);

  tmp = swift_weakLoadStrong(&ref1);
  ASSERT_EQ(nullptr, tmp);
  swift_release(tmp);

  WeakReference ref2;
  res = swift_weakCopyInit(&ref2, &ref1);
  ASSERT_EQ(res, &ref2);

  WeakReference ref3;
  res = swift_weakTakeInit(&ref3, &ref2);
  ASSERT_EQ(res, &ref3);

  HeapObject *o3 = make_swift_object();
  WeakReference ref4; // ref4 = init
  res = swift_weakInit(&ref4, o3);
  ASSERT_EQ(res, &ref4);

  res = swift_weakCopyAssign(&ref4, &ref3);
  ASSERT_EQ(res, &ref4);

  res = swift_weakTakeAssign(&ref4, &ref3);
  ASSERT_EQ(res, &ref4);

  swift_weakDestroy(&ref4);
  swift_weakDestroy(&ref1);
  swift_release(o3);
}

TEST(WeakTest, simple_objc) {
  void *o1 = make_objc_object();
  void *o2 = make_objc_object();
  ASSERT_NE(o1, o2);
  ASSERT_NE(o1, nullptr);
  ASSERT_NE(o2, nullptr);

  DestroyedObjCCount = 0;

  WeakReference ref1;
  swift_unknownObjectWeakInit(&ref1, o1);

  void *tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(tmp, o1);
  unknown_release(tmp);

  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(o1, tmp);
  unknown_release(tmp);
  ASSERT_EQ(0U, DestroyedObjCCount);

  swift_unknownObjectWeakAssign(&ref1, o2);
  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(o2, tmp);
  unknown_release(tmp);
  ASSERT_EQ(0U, DestroyedObjCCount);

  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(o2, tmp);
  unknown_release(tmp);
  ASSERT_EQ(0U, DestroyedObjCCount);

  unknown_release(o1);
  ASSERT_EQ(1U, DestroyedObjCCount);

  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(o2, tmp);
  unknown_release(tmp);
  ASSERT_EQ(1U, DestroyedObjCCount);

  unknown_release(o2);
  ASSERT_EQ(2U, DestroyedObjCCount);

  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(nullptr, tmp);
  unknown_release(tmp);
  ASSERT_EQ(2U, DestroyedObjCCount);

  swift_unknownObjectWeakDestroy(&ref1);
}

TEST(WeakTest, simple_swift_as_unknown) {
  void *o1 = make_swift_object();
  void *o2 = make_swift_object();
  ASSERT_NE(o1, o2);
  ASSERT_NE(o1, nullptr);
  ASSERT_NE(o2, nullptr);

  WeakReference ref1;
  swift_unknownObjectWeakInit(&ref1, o1);

  void *tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(tmp, o1);
  unknown_release(tmp);

  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(o1, tmp);
  unknown_release(tmp);

  swift_unknownObjectWeakAssign(&ref1, o2);
  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(o2, tmp);
  unknown_release(tmp);

  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(o2, tmp);
  unknown_release(tmp);

  unknown_release(o1);

  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(o2, tmp);
  unknown_release(tmp);

  unknown_release(o2);

  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(nullptr, tmp);
  unknown_release(tmp);

  swift_unknownObjectWeakDestroy(&ref1);
}

TEST(WeakTest, simple_swift_and_objc) {
  void *o1 = make_swift_object();
  void *o2 = make_objc_object();
  ASSERT_NE(o1, o2);
  ASSERT_NE(o1, nullptr);
  ASSERT_NE(o2, nullptr);

  DestroyedObjCCount = 0;

  WeakReference ref1;
  swift_unknownObjectWeakInit(&ref1, o1);

  void *tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(tmp, o1);
  unknown_release(tmp);

  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(o1, tmp);
  unknown_release(tmp);
  ASSERT_EQ(0U, DestroyedObjCCount);

  swift_unknownObjectWeakAssign(&ref1, o2);
  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(o2, tmp);
  unknown_release(tmp);
  ASSERT_EQ(0U, DestroyedObjCCount);

  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(o2, tmp);
  unknown_release(tmp);
  ASSERT_EQ(0U, DestroyedObjCCount);

  unknown_release(o1);
  ASSERT_EQ(0U, DestroyedObjCCount);

  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(o2, tmp);
  unknown_release(tmp);
  ASSERT_EQ(0U, DestroyedObjCCount);

  unknown_release(o2);
  ASSERT_EQ(1U, DestroyedObjCCount);

  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(nullptr, tmp);
  unknown_release(tmp);
  ASSERT_EQ(1U, DestroyedObjCCount);

  swift_unknownObjectWeakDestroy(&ref1);
}

TEST(WeakTest, simple_objc_and_swift) {
  void *o1 = make_objc_object();
  void *o2 = make_swift_object();
  ASSERT_NE(o1, o2);
  ASSERT_NE(o1, nullptr);
  ASSERT_NE(o2, nullptr);

  DestroyedObjCCount = 0;

  WeakReference ref1;
  auto res = swift_unknownObjectWeakInit(&ref1, o1);
  ASSERT_EQ(&ref1, res);

  void *tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(tmp, o1);
  unknown_release(tmp);

  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(o1, tmp);
  unknown_release(tmp);
  ASSERT_EQ(0U, DestroyedObjCCount);

  swift_unknownObjectWeakAssign(&ref1, o2);
  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(o2, tmp);
  unknown_release(tmp);
  ASSERT_EQ(0U, DestroyedObjCCount);

  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(o2, tmp);
  unknown_release(tmp);
  ASSERT_EQ(0U, DestroyedObjCCount);

  unknown_release(o1);
  ASSERT_EQ(1U, DestroyedObjCCount);

  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(o2, tmp);
  unknown_release(tmp);
  ASSERT_EQ(1U, DestroyedObjCCount);

  unknown_release(o2);
  ASSERT_EQ(1U, DestroyedObjCCount);

  tmp = swift_unknownObjectWeakLoadStrong(&ref1);
  ASSERT_EQ(nullptr, tmp);
  unknown_release(tmp);
  ASSERT_EQ(1U, DestroyedObjCCount);

  swift_unknownObjectWeakDestroy(&ref1);
}

TEST(WeakTest, objc_unowned_basic) {
  UnownedReference ref;
  void *objc1 = make_objc_object();
  void *objc2 = make_objc_object();
  HeapObject *swift1 = make_swift_object();
  HeapObject *swift2 = make_swift_object();

  ASSERT_NE(objc1, objc2);
  ASSERT_NE(swift1, swift2);
  ASSERT_EQ(0U, getUnownedRetainCount(swift1));
  ASSERT_EQ(0U, getUnownedRetainCount(swift2));

  void *result;

  // ref = swift1
  swift_unknownObjectUnownedInit(&ref, swift1);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  result = swift_unknownObjectUnownedLoadStrong(&ref);
  ASSERT_EQ(swift1, result);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  swift_unknownObjectRelease(result);
  swift_unknownObjectUnownedDestroy(&ref);
  ASSERT_EQ(0U, getUnownedRetainCount(swift1));

  // ref = objc1
  swift_unknownObjectUnownedInit(&ref, objc1);
  result = swift_unknownObjectUnownedLoadStrong(&ref);
  ASSERT_EQ(objc1, result);
  swift_unknownObjectRelease(result);

  // ref = objc1 (objc self transition)
  swift_unknownObjectUnownedAssign(&ref, objc1);
  result = swift_unknownObjectUnownedLoadStrong(&ref);
  ASSERT_EQ(objc1, result);
  swift_unknownObjectRelease(result);

  // ref = objc2 (objc -> objc transition)
  swift_unknownObjectUnownedAssign(&ref, objc2);
  result = swift_unknownObjectUnownedLoadStrong(&ref);
  ASSERT_EQ(objc2, result);
  swift_unknownObjectRelease(result);

  // ref = swift1 (objc -> swift transition)
  swift_unknownObjectUnownedAssign(&ref, swift1);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  result = swift_unknownObjectUnownedLoadStrong(&ref);
  ASSERT_EQ(swift1, result);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  swift_unknownObjectRelease(result);

  // ref = swift1 (swift self transition)
  swift_unknownObjectUnownedAssign(&ref, swift1);
  result = swift_unknownObjectUnownedLoadStrong(&ref);
  ASSERT_EQ(swift1, result);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  swift_unknownObjectRelease(result);

  // ref = swift2 (swift -> swift transition)
  swift_unknownObjectUnownedAssign(&ref, swift2);
  result = swift_unknownObjectUnownedLoadStrong(&ref);
  ASSERT_EQ(swift2, result);
  ASSERT_EQ(0U, getUnownedRetainCount(swift1));
  ASSERT_EQ(1U, getUnownedRetainCount(swift2));
  swift_unknownObjectRelease(result);

  // ref = objc1 (swift -> objc transition)
  swift_unknownObjectUnownedAssign(&ref, objc1);
  result = swift_unknownObjectUnownedLoadStrong(&ref);
  ASSERT_EQ(objc1, result);
  ASSERT_EQ(0U, getUnownedRetainCount(swift2));
  swift_unknownObjectRelease(result);

  swift_unknownObjectUnownedDestroy(&ref);

  swift_unknownObjectRelease(objc1);
  swift_unknownObjectRelease(objc2);
  swift_unknownObjectRelease(swift1);
  swift_unknownObjectRelease(swift2);
}

TEST(WeakTest, objc_unowned_takeStrong) {
  UnownedReference ref;
  void *objc1 = make_objc_object();
  HeapObject *swift1 = make_swift_object();

  void *result;

  // ref = objc1
  swift_unknownObjectUnownedInit(&ref, objc1);
  result = swift_unknownObjectUnownedTakeStrong(&ref);
  ASSERT_EQ(objc1, result);
  swift_unknownObjectRelease(result);

  // ref = swift1
  swift_unknownObjectUnownedInit(&ref, swift1);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  result = swift_unknownObjectUnownedTakeStrong(&ref);
  ASSERT_EQ(swift1, result);
  ASSERT_EQ(0U, getUnownedRetainCount(swift1));
  swift_unknownObjectRelease(result);

  swift_unknownObjectRelease(objc1);
  swift_unknownObjectRelease(swift1);
}

TEST(WeakTest, objc_unowned_copyInit_nil) {
  UnownedReference ref1;
  UnownedReference ref2;

  void *result;

  // ref1 = nil
  swift_unknownObjectUnownedInit(&ref1, nullptr);
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(nullptr, result);

  // ref2 = ref1 (nil -> nil)
  auto res = swift_unknownObjectUnownedCopyInit(&ref2, &ref1);
  ASSERT_EQ(&ref2, res);
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(nullptr, result);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(nullptr, result);
  swift_unknownObjectUnownedDestroy(&ref2);
}

TEST(WeakTest, objc_unowned_copyInit_objc) {
  UnownedReference ref1;
  UnownedReference ref2;

  void *result;
  void *objc1 = make_objc_object();

  // ref1 = objc1
  swift_unknownObjectUnownedInit(&ref1, objc1);
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(objc1, result);
  swift_unknownObjectRelease(result);

  // ref2 = ref1 (objc -> objc)
  swift_unknownObjectUnownedCopyInit(&ref2, &ref1);
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(objc1, result);
  swift_unknownObjectRelease(result);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(objc1, result);
  swift_unknownObjectRelease(result);
  swift_unknownObjectUnownedDestroy(&ref2);

  swift_unknownObjectUnownedDestroy(&ref1);

  swift_unknownObjectRelease(objc1);
}

TEST(WeakTest, objc_unowned_copyInit_swift) {
  UnownedReference ref1;
  UnownedReference ref2;

  void *result;

  HeapObject *swift1 = make_swift_object();
  ASSERT_EQ(0U, getUnownedRetainCount(swift1));

  // ref1 = swift1
  swift_unknownObjectUnownedInit(&ref1, swift1);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(swift1, result);
  swift_unknownObjectRelease(result);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));

  // ref2 = ref1 (swift -> swift)
  swift_unknownObjectUnownedCopyInit(&ref2, &ref1);
  ASSERT_EQ(2U, getUnownedRetainCount(swift1));
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(swift1, result);
  swift_unknownObjectRelease(result);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(swift1, result);
  swift_unknownObjectRelease(result);
  ASSERT_EQ(2U, getUnownedRetainCount(swift1));
  swift_unknownObjectUnownedDestroy(&ref2);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));

  // ref2 = ref1
  // ref2 = nil
  swift_unknownObjectUnownedCopyInit(&ref2, &ref1);
  ASSERT_EQ(2U, getUnownedRetainCount(swift1));
  swift_unknownObjectUnownedAssign(&ref2, nullptr);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(swift1, result);
  swift_unknownObjectRelease(result);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(nullptr, result);
  swift_unknownObjectRelease(result);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  swift_unknownObjectUnownedDestroy(&ref2);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));

  swift_unknownObjectUnownedDestroy(&ref1);
  ASSERT_EQ(0U, getUnownedRetainCount(swift1));

  swift_unknownObjectRelease(swift1);
}

TEST(WeakTest, objc_unowned_takeInit_nil) {
  UnownedReference ref1;
  UnownedReference ref2;

  void *result;

  // ref1 = nil
  swift_unknownObjectUnownedInit(&ref1, nullptr);
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(nullptr, result);

  // ref2 = ref1 (nil -> nil)
  auto res = swift_unknownObjectUnownedTakeInit(&ref2, &ref1);
  ASSERT_EQ(&ref2, res);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(nullptr, result);
  swift_unknownObjectUnownedDestroy(&ref2);
}

TEST(WeakTest, objc_unowned_takeInit_objc) {
  UnownedReference ref1;
  UnownedReference ref2;

  void *result;
  void *objc1 = make_objc_object();

  // ref1 = objc1
  swift_unknownObjectUnownedInit(&ref1, objc1);
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(objc1, result);
  swift_unknownObjectRelease(result);

  // ref2 = ref1 (objc -> objc)
  swift_unknownObjectUnownedTakeInit(&ref2, &ref1);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(objc1, result);
  swift_unknownObjectRelease(result);
  swift_unknownObjectUnownedDestroy(&ref2);

  swift_unknownObjectRelease(objc1);
}

TEST(WeakTest, objc_unowned_takeInit_swift) {
  UnownedReference ref1;
  UnownedReference ref2;

  void *result;

  HeapObject *swift1 = make_swift_object();
  ASSERT_EQ(0U, getUnownedRetainCount(swift1));

  // ref1 = swift1
  swift_unknownObjectUnownedInit(&ref1, swift1);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(swift1, result);
  swift_unknownObjectRelease(result);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));

  // ref2 = ref1 (swift -> swift)
  swift_unknownObjectUnownedTakeInit(&ref2, &ref1);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(swift1, result);
  swift_unknownObjectRelease(result);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  swift_unknownObjectUnownedDestroy(&ref2);
  ASSERT_EQ(0U, getUnownedRetainCount(swift1));

  // ref1 = swift1
  swift_unknownObjectUnownedInit(&ref1, swift1);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));

  // ref2 = ref1
  // ref2 = nil
  swift_unknownObjectUnownedTakeInit(&ref2, &ref1);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  swift_unknownObjectUnownedAssign(&ref2, nullptr);
  ASSERT_EQ(0U, getUnownedRetainCount(swift1));
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(nullptr, result);

  swift_unknownObjectUnownedDestroy(&ref2);
  ASSERT_EQ(0U, getUnownedRetainCount(swift1));

  swift_unknownObjectRelease(swift1);
}

TEST(WeakTest, objc_unowned_copyAssign) {
  UnownedReference ref1;
  UnownedReference ref2;
  void *objc1 = make_objc_object();
  void *objc2 = make_objc_object();
  HeapObject *swift1 = make_swift_object();
  HeapObject *swift2 = make_swift_object();

  ASSERT_NE(objc1, objc2);
  ASSERT_NE(swift1, swift2);
  ASSERT_EQ(0U, getUnownedRetainCount(swift1));
  ASSERT_EQ(0U, getUnownedRetainCount(swift2));

  void *result;

  // ref1 = objc1
  swift_unknownObjectUnownedInit(&ref1, objc1);
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(objc1, result);
  swift_unknownObjectRelease(result);

  // ref2 = objc1
  swift_unknownObjectUnownedInit(&ref2, objc1);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(objc1, result);
  swift_unknownObjectRelease(result);

  // ref1 = ref2 (objc self transition)
  auto res = swift_unknownObjectUnownedCopyAssign(&ref1, &ref2);
  ASSERT_EQ(&ref1, res);
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(objc1, result);
  swift_unknownObjectRelease(result);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(objc1, result);
  swift_unknownObjectRelease(result);

  // ref2 = objc2
  swift_unknownObjectUnownedAssign(&ref2, objc2);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(objc2, result);
  swift_unknownObjectRelease(result);

  // ref1 = ref2 (objc -> objc transition)
  swift_unknownObjectUnownedCopyAssign(&ref1, &ref2);
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(objc2, result);
  swift_unknownObjectRelease(result);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(objc2, result);
  swift_unknownObjectRelease(result);

  // ref2 = swift1
  swift_unknownObjectUnownedAssign(&ref2, swift1);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(swift1, result);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  swift_unknownObjectRelease(result);

  // ref1 = ref2 (objc -> swift transition)
  swift_unknownObjectUnownedCopyAssign(&ref1, &ref2);
  ASSERT_EQ(2U, getUnownedRetainCount(swift1));
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(swift1, result);
  swift_unknownObjectRelease(result);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(swift1, result);
  swift_unknownObjectRelease(result);

  // ref2 = swift1
  swift_unknownObjectUnownedAssign(&ref2, swift1);
  ASSERT_EQ(2U, getUnownedRetainCount(swift1));
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(swift1, result);
  swift_unknownObjectRelease(result);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(swift1, result);
  swift_unknownObjectRelease(result);
  ASSERT_EQ(2U, getUnownedRetainCount(swift1));

  // ref1 = ref2 (swift self transition)
  swift_unknownObjectUnownedCopyAssign(&ref1, &ref2);
  ASSERT_EQ(2U, getUnownedRetainCount(swift1));
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(swift1, result);
  ASSERT_EQ(2U, getUnownedRetainCount(swift1));
  swift_unknownObjectRelease(result);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(swift1, result);
  ASSERT_EQ(2U, getUnownedRetainCount(swift1));
  swift_unknownObjectRelease(result);

  // ref2 = swift2
  swift_unknownObjectUnownedAssign(&ref2, swift2);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  ASSERT_EQ(1U, getUnownedRetainCount(swift2));
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(swift2, result);
  swift_unknownObjectRelease(result);

  // ref1 = ref2 (swift -> swift transition)
  swift_unknownObjectUnownedCopyAssign(&ref1, &ref2);
  ASSERT_EQ(0U, getUnownedRetainCount(swift1));
  ASSERT_EQ(2U, getUnownedRetainCount(swift2));
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(swift2, result);
  ASSERT_EQ(2U, getUnownedRetainCount(swift2));
  swift_unknownObjectRelease(result);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(swift2, result);
  ASSERT_EQ(2U, getUnownedRetainCount(swift2));
  swift_unknownObjectRelease(result);

  // ref2 = objc1
  swift_unknownObjectUnownedAssign(&ref2, objc1);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(objc1, result);
  ASSERT_EQ(1U, getUnownedRetainCount(swift2));
  swift_unknownObjectRelease(result);

  // ref1 = ref2 (swift -> objc transition)
  swift_unknownObjectUnownedCopyAssign(&ref1, &ref2);
  ASSERT_EQ(0U, getUnownedRetainCount(swift1));
  ASSERT_EQ(0U, getUnownedRetainCount(swift2));
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(objc1, result);
  swift_unknownObjectRelease(result);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(objc1, result);
  swift_unknownObjectRelease(result);

  swift_unknownObjectUnownedDestroy(&ref1);
  swift_unknownObjectUnownedDestroy(&ref2);

  swift_unknownObjectRelease(objc1);
  swift_unknownObjectRelease(objc2);
  swift_unknownObjectRelease(swift1);
  swift_unknownObjectRelease(swift2);
}

TEST(WeakTest, objc_unowned_takeAssign) {
  UnownedReference ref1;
  UnownedReference ref2;
  void *objc1 = make_objc_object();
  void *objc2 = make_objc_object();
  HeapObject *swift1 = make_swift_object();
  HeapObject *swift2 = make_swift_object();

  ASSERT_NE(objc1, objc2);
  ASSERT_NE(swift1, swift2);
  ASSERT_EQ(0U, getUnownedRetainCount(swift1));
  ASSERT_EQ(0U, getUnownedRetainCount(swift2));

  void *result;

  // ref1 = objc1
  auto res = swift_unknownObjectUnownedInit(&ref1, objc1);
  ASSERT_EQ(&ref1, res);
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(objc1, result);
  swift_unknownObjectRelease(result);

  // ref2 = objc1
  swift_unknownObjectUnownedInit(&ref2, objc1);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(objc1, result);
  swift_unknownObjectRelease(result);

  // ref1 = ref2 (objc self transition)
  res = swift_unknownObjectUnownedTakeAssign(&ref1, &ref2);
  ASSERT_EQ(&ref1, res);
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(objc1, result);
  swift_unknownObjectRelease(result);

  // ref2 = objc2
  swift_unknownObjectUnownedInit(&ref2, objc2);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(objc2, result);
  swift_unknownObjectRelease(result);

  // ref1 = ref2 (objc -> objc transition)
  swift_unknownObjectUnownedTakeAssign(&ref1, &ref2);
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(objc2, result);
  swift_unknownObjectRelease(result);

  // ref2 = swift1
  swift_unknownObjectUnownedInit(&ref2, swift1);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(swift1, result);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  swift_unknownObjectRelease(result);

  // ref1 = ref2 (objc -> swift transition)
  swift_unknownObjectUnownedTakeAssign(&ref1, &ref2);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(swift1, result);
  swift_unknownObjectRelease(result);

  // ref2 = swift1
  swift_unknownObjectUnownedInit(&ref2, swift1);
  ASSERT_EQ(2U, getUnownedRetainCount(swift1));
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(swift1, result);
  swift_unknownObjectRelease(result);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(swift1, result);
  swift_unknownObjectRelease(result);
  ASSERT_EQ(2U, getUnownedRetainCount(swift1));

  // ref1 = ref2 (swift self transition)
  swift_unknownObjectUnownedTakeAssign(&ref1, &ref2);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(swift1, result);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  swift_unknownObjectRelease(result);

  // ref2 = swift2
  swift_unknownObjectUnownedInit(&ref2, swift2);
  ASSERT_EQ(1U, getUnownedRetainCount(swift1));
  ASSERT_EQ(1U, getUnownedRetainCount(swift2));
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(swift2, result);
  swift_unknownObjectRelease(result);

  // ref1 = ref2 (swift -> swift transition)
  swift_unknownObjectUnownedTakeAssign(&ref1, &ref2);
  ASSERT_EQ(0U, getUnownedRetainCount(swift1));
  ASSERT_EQ(1U, getUnownedRetainCount(swift2));
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(swift2, result);
  ASSERT_EQ(1U, getUnownedRetainCount(swift2));
  swift_unknownObjectRelease(result);

  // ref2 = objc1
  swift_unknownObjectUnownedInit(&ref2, objc1);
  result = swift_unknownObjectUnownedLoadStrong(&ref2);
  ASSERT_EQ(objc1, result);
  ASSERT_EQ(1U, getUnownedRetainCount(swift2));
  swift_unknownObjectRelease(result);

  // ref1 = ref2 (swift -> objc transition)
  swift_unknownObjectUnownedTakeAssign(&ref1, &ref2);
  ASSERT_EQ(0U, getUnownedRetainCount(swift1));
  ASSERT_EQ(0U, getUnownedRetainCount(swift2));
  result = swift_unknownObjectUnownedLoadStrong(&ref1);
  ASSERT_EQ(objc1, result);
  swift_unknownObjectRelease(result);

  swift_unknownObjectUnownedDestroy(&ref1);

  swift_unknownObjectRelease(objc1);
  swift_unknownObjectRelease(objc2);
  swift_unknownObjectRelease(swift1);
  swift_unknownObjectRelease(swift2);
}

TEST(WeakTest, objc_unowned_isEqual_DeathTest) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";

  DestroyedObjCCount = 0;

  UnownedReference ref1;
  UnownedReference ref2;
  void *objc1 = make_objc_object();
  void *objc2 = make_objc_object();
  HeapObject *swift1 = make_swift_object();
  HeapObject *swift2 = make_swift_object();

  // ref1 = swift1
  swift_unownedInit(&ref1, swift1);
  ASSERT_EQ(true,  swift_unownedIsEqual(&ref1, swift1));
  ASSERT_EQ(false, swift_unownedIsEqual(&ref1, swift2));
  ASSERT_EQ(true,  swift_unknownObjectUnownedIsEqual(&ref1, swift1));
  ASSERT_EQ(false, swift_unknownObjectUnownedIsEqual(&ref1, swift2));
  ASSERT_EQ(false, swift_unknownObjectUnownedIsEqual(&ref1, objc1));
  ASSERT_EQ(false, swift_unknownObjectUnownedIsEqual(&ref1, objc2));

  // ref2 = objc1
  swift_unknownObjectUnownedInit(&ref2, objc1);
  ASSERT_EQ(false, swift_unknownObjectUnownedIsEqual(&ref2, swift1));
  ASSERT_EQ(false, swift_unknownObjectUnownedIsEqual(&ref2, swift2));
  ASSERT_EQ(true,  swift_unknownObjectUnownedIsEqual(&ref2, objc1));
  ASSERT_EQ(false, swift_unknownObjectUnownedIsEqual(&ref2, objc2));

  // Deinit the assigned objects, invalidating ref1 and ref2
  swift_release(swift1);
  ASSERT_DEATH(swift_unownedCheck(swift1),
               "Attempted to read an unowned reference");
  ASSERT_EQ(0U, DestroyedObjCCount);
  swift_unknownObjectRelease(objc1);
  ASSERT_EQ(1U, DestroyedObjCCount);

  // Unequal does not abort, even after invalidation
  // Equal but invalidated does abort (Swift)
  // Formerly equal but now invalidated returns unequal (ObjC)
  ASSERT_DEATH(swift_unownedIsEqual(&ref1, swift1),
               "Attempted to read an unowned reference");
  ASSERT_EQ(false, swift_unownedIsEqual(&ref1, swift2));
  ASSERT_DEATH(swift_unknownObjectUnownedIsEqual(&ref1, swift1),
               "Attempted to read an unowned reference");
  ASSERT_EQ(false, swift_unknownObjectUnownedIsEqual(&ref1, swift2));
  ASSERT_EQ(false, swift_unknownObjectUnownedIsEqual(&ref1, objc1));
  ASSERT_EQ(false, swift_unknownObjectUnownedIsEqual(&ref1, objc2));

  ASSERT_EQ(false, swift_unknownObjectUnownedIsEqual(&ref2, swift1));
  ASSERT_EQ(false, swift_unknownObjectUnownedIsEqual(&ref2, swift2));
  ASSERT_EQ(false, swift_unknownObjectUnownedIsEqual(&ref2, objc1));
  ASSERT_EQ(false, swift_unknownObjectUnownedIsEqual(&ref2, objc2));

  swift_release(swift2);
  swift_unknownObjectRelease(objc2);

  swift_unownedDestroy(&ref1);
  swift_unknownObjectUnownedDestroy(&ref2);
}

TEST(WeakTest, unknownWeak) {
  void *objc1 = make_objc_object();
  HeapObject *swift1 = make_swift_object();

  WeakReference ref1;
  auto res = swift_unknownObjectWeakInit(&ref1, objc1);
  ASSERT_EQ(&ref1, res);

  WeakReference ref2;
  res = swift_unknownObjectWeakCopyInit(&ref2, &ref1);
  ASSERT_EQ(&ref2, res);

  WeakReference ref3; // ref2 dead.
  res = swift_unknownObjectWeakTakeInit(&ref3, &ref2);
  ASSERT_EQ(&ref3, res);

  res = swift_unknownObjectWeakAssign(&ref3, swift1);
  ASSERT_EQ(&ref3, res);

  res = swift_unknownObjectWeakCopyAssign(&ref3, &ref1);
  ASSERT_EQ(&ref3, res);

  res = swift_unknownObjectWeakTakeAssign(&ref3, &ref1);
  ASSERT_EQ(&ref3, res);

  swift_unknownObjectWeakDestroy(&ref3);

  swift_release(swift1);
  swift_unknownObjectRelease(objc1);
}

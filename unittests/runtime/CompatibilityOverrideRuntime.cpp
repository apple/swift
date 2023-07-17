//===--- CompatibilityOverrideRuntime.cpp - Compatibility override tests ---===//
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

#if defined(__APPLE__) && defined(__MACH__)

#define SWIFT_TARGET_LIBRARY_NAME swiftRuntime

#include "../../stdlib/public/CompatibilityOverride/CompatibilityOverride.h"
#include "swift/Runtime/Casting.h"
#include "swift/Runtime/Metadata.h"
#include "gtest/gtest.h"

#include <stdio.h>

using namespace swift;

bool EnableOverride;
bool Ran;

namespace  {
  template<typename T>
  T getEmptyValue() {
    return (T)0;
  }

  template<>
  MetadataResponse getEmptyValue<MetadataResponse>() {
    return MetadataResponse{nullptr, MetadataState::Complete};
  }

  template <>
  TypeLookupErrorOr<TypeInfo> getEmptyValue<TypeLookupErrorOr<TypeInfo>>() {
    return TypeInfo();
  }
}

#define OVERRIDE(name, ret, attrs, ccAttrs, namespace, typedArgs, namedArgs)   \
  static ccAttrs ret name##Override(COMPATIBILITY_UNPAREN_WITH_COMMA(          \
      typedArgs) Original_##name originalImpl) {                               \
    if (!EnableOverride)                                                       \
      return originalImpl COMPATIBILITY_PAREN(namedArgs);                      \
    Ran = true;                                                                \
    return getEmptyValue<ret>();                                               \
  }
#include "../../stdlib/public/CompatibilityOverride/CompatibilityOverrideRuntime.def"

struct OverrideSection {
  uintptr_t version;
  
#define OVERRIDE(name, ret, attrs, ccAttrs, namespace, typedArgs, namedArgs) \
  Override_ ## name name;
#include "../../stdlib/public/CompatibilityOverride/CompatibilityOverrideRuntime.def"
};

OverrideSection RuntimeOverrides
    __attribute__((section("__DATA," COMPATIBILITY_OVERRIDE_SECTION_NAME_swiftRuntime))) = {
        0,
#define OVERRIDE(name, ret, attrs, ccAttrs, namespace, typedArgs, namedArgs) \
  name ## Override,
#include "../../stdlib/public/CompatibilityOverride/CompatibilityOverrideRuntime.def"
};

class CompatibilityOverrideRuntimeTest : public ::testing::Test {
protected:
  virtual void SetUp() {
    // This check is a bit pointless, as long as you trust the section
    // attribute to work correctly, but it ensures that the override
    // section actually gets linked into the test executable. If it's not
    // used then it disappears and the real tests begin to fail.
    ASSERT_EQ(RuntimeOverrides.version, static_cast<uintptr_t>(0));

    EnableOverride = true;
    Ran = false;
  }
  
  virtual void TearDown() {
    EnableOverride = false;
    ASSERT_TRUE(Ran);
  }
};

TEST_F(CompatibilityOverrideRuntimeTest, test_swift_dynamicCast) {
  auto Result = swift_dynamicCast(nullptr, nullptr, nullptr, nullptr,
                                  DynamicCastFlags::Default);
  ASSERT_FALSE(Result);
}

TEST_F(CompatibilityOverrideRuntimeTest, test_swift_dynamicCastClass) {
  auto Result = swift_dynamicCastClass(nullptr, nullptr);
  ASSERT_EQ(Result, nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest,
       test_swift_dynamicCastClassUnconditional) {
  auto Result = swift_dynamicCastClassUnconditional(nullptr, nullptr, nullptr, 0, 0);
  ASSERT_EQ(Result, nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest, test_swift_dynamicCastObjCClass) {
  auto Result = swift_dynamicCastObjCClass(nullptr, nullptr);
  ASSERT_EQ(Result, nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest,
       test_swift_dynamicCastObjCClassUnconditional) {
  auto Result = swift_dynamicCastObjCClassUnconditional(nullptr, nullptr, nullptr, 0, 0);
  ASSERT_EQ(Result, nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest, test_swift_dynamicCastForeignClass) {
  auto Result = swift_dynamicCastForeignClass(nullptr, nullptr);
  ASSERT_EQ(Result, nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest,
       test_swift_dynamicCastForeignClassUnconditional) {
  auto Result = swift_dynamicCastForeignClassUnconditional(nullptr, nullptr, nullptr, 0, 0);
  ASSERT_EQ(Result, nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest, test_swift_dynamicCastUnknownClass) {
  auto Result = swift_dynamicCastUnknownClass(nullptr, nullptr);
  ASSERT_EQ(Result, nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest,
       test_swift_dynamicCastUnknownClassUnconditional) {
  auto Result = swift_dynamicCastUnknownClassUnconditional(nullptr, nullptr, nullptr, 0, 0);
  ASSERT_EQ(Result, nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest, test_swift_dynamicCastMetatype) {
  auto Result = swift_dynamicCastMetatype(nullptr, nullptr);
  ASSERT_EQ(Result, nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest,
       test_swift_dynamicCastMetatypeUnconditional) {
  auto Result = swift_dynamicCastMetatypeUnconditional(nullptr, nullptr, nullptr, 0, 0);
  ASSERT_EQ(Result, nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest,
       test_swift_dynamicCastObjCClassMetatype) {
  auto Result = swift_dynamicCastObjCClassMetatype(nullptr, nullptr);
  ASSERT_EQ(Result, nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest,
       test_swift_dynamicCastObjCClassMetatypeUnconditional) {
  auto Result = swift_dynamicCastObjCClassMetatypeUnconditional(nullptr, nullptr, nullptr, 0, 0);
  ASSERT_EQ(Result, nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest,
       test_swift_dynamicCastForeignClassMetatype) {
  auto Result = swift_dynamicCastForeignClassMetatype(nullptr, nullptr);
  ASSERT_EQ(Result, nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest,
       test_swift_dynamicCastForeignClassMetatypeUnconditional) {
  auto Result = swift_dynamicCastForeignClassMetatypeUnconditional(nullptr, nullptr, nullptr, 0, 0);
  ASSERT_EQ(Result, nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest, test_swift_conformsToProtocol) {
  auto Result = swift_conformsToProtocol(nullptr, nullptr);
  ASSERT_EQ(Result, nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest, test_swift_conformsToProtocol2) {
  auto Result = swift_conformsToProtocol2(nullptr, nullptr);
  ASSERT_EQ(Result, nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest, test_swift_conformsToProtocolCommon) {
  auto Result = swift_conformsToProtocolCommon(nullptr, nullptr);
  ASSERT_EQ(Result, nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest, test_swift_getTypeByMangledNode) {
  Demangler demangler;
  auto Result = swift_getTypeByMangledNode(MetadataState::Abstract,
                                           demangler, nullptr, nullptr, nullptr,nullptr);
  ASSERT_EQ(Result.getType().getMetadata(), nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest, test_swift_getTypeByMangledName) {
  auto Result = swift_getTypeByMangledName(MetadataState::Abstract,
                                           "", nullptr, nullptr, nullptr);
  ASSERT_EQ(Result.getType().getMetadata(), nullptr);
}

TEST_F(CompatibilityOverrideRuntimeTest,
       test_swift_getAssociatedTypeWitnessSlow) {
  auto Result = swift_getAssociatedTypeWitnessSlow(MetadataState::Complete,
                                                   nullptr, nullptr,
                                                   nullptr, nullptr);
  ASSERT_EQ(Result.Value, nullptr);
  ASSERT_EQ(Result.State, MetadataState::Complete);
}

TEST_F(CompatibilityOverrideRuntimeTest,
       test_swift_getAssociatedConformanceWitnessSlow) {
  auto Result = swift_getAssociatedConformanceWitnessSlow(
                                                   nullptr, nullptr, nullptr,
                                                   nullptr, nullptr);
  ASSERT_EQ(Result, nullptr);
}

#endif

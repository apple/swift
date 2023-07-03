//===--- BlotMapVectorTest.cpp --------------------------------------------===//
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

#define DEBUG_TYPE "swift-blot-map-vector-test"
#include "swift/Basic/BlotMapVector.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Lazy.h"
#include "swift/Basic/NullablePtr.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"
#include "gtest/gtest.h"
#include <map>
#include <set>

using namespace swift;

//===----------------------------------------------------------------------===//
//                         Test Key/Value Generation
//===----------------------------------------------------------------------===//

namespace {

uint32_t getTestKey(int i, uint32_t *) { return i; }
uint32_t getTestValue(int i, uint32_t *) { return 42 + i; }

uint32_t *getTestKey(int i, uint32_t **) {
  static uint32_t dummy_arr1[8192];
  assert(i < 8192 && "Only support 8192 dummy keys.");
  return &dummy_arr1[i];
}

uint32_t *getTestValue(int i, uint32_t **) {
  static uint32_t dummy_arr1[8192];
  assert(i < 8192 && "Only support 8192 dummy keys.");
  return &dummy_arr1[i];
}

} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                                 CtorTester
//===----------------------------------------------------------------------===//

namespace {

class CtorTester;

class CtorTesterSet {
  bool IsClearing = false;
  std::set<CtorTester *> Constructed;

public:
  void init() { IsClearing = false; }
  void dumpLiveTesters() const;
  void verifyTesters() const;
  bool hasLiveTesters() const;
  bool numLiveTesters() const;
  void clearTesters();
  void finalize();
  bool isLive(CtorTester *T) const;
  bool insert(CtorTester *T);
  unsigned erase(CtorTester *T);
  bool isClearing() const { return IsClearing; }
};

Lazy<CtorTesterSet> ConstructedTesters;

/// A test class that tries to check that construction and destruction
/// occur correctly.
class CtorTester {
  friend class CtorTesterSet;

  NullablePtr<int> Value;

public:
  bool isLive() const {
    return ConstructedTesters->isLive(const_cast<CtorTester *>(this));
  }

  enum class EmptyTester { Kind };
  enum class TombstoneTester { Kind };

  CtorTester() : Value(new int(-1)) {
    dump("Constructing ()");
    LLVM_DEBUG(llvm::errs() << "\n");
    // EXPECT_TRUE(ConstructedTesters->insert(this));
    assert(!isLive());
    fflush(stdout);
  }

  explicit CtorTester(EmptyTester) : Value(new int(-2)) {
    dump("Constructing Empty");
    LLVM_DEBUG(llvm::errs() << "\n");
    // EXPECT_TRUE(ConstructedTesters->insert(this));
    assert(!isLive());
    fflush(stdout);
  }

  explicit CtorTester(TombstoneTester) : Value(new int(-3)) {
    dump("Constructing Tombstone");
    LLVM_DEBUG(llvm::errs() << "\n");
    // EXPECT_TRUE(ConstructedTesters->insert(this));
    assert(!isLive());
    fflush(stdout);
  }

  explicit CtorTester(int V) : Value(new int(V)) {
    dump("Constructing Normal");
    LLVM_DEBUG(llvm::errs() << "\n");
    EXPECT_TRUE(ConstructedTesters->insert(this));
    assert(!isIgnorableTester());
    assert(isLive());
    fflush(stdout);
  }

  explicit CtorTester(uint32_t V) : Value(new int(V)) {
    dump("Constructing Normal");
    LLVM_DEBUG(llvm::errs() << "\n");
    EXPECT_TRUE(ConstructedTesters->insert(this));
    assert(!isIgnorableTester());
    assert(isLive());
    fflush(stdout);
  }

  CtorTester(const CtorTester &Arg) : Value(new int(*Arg.Value.get())) {
    dump("CopyConstructing");
    Arg.dump("   From");
    LLVM_DEBUG(llvm::errs() << "\n");
    if (!Arg.isIgnorableTester()) {
      EXPECT_TRUE(ConstructedTesters->insert(this));
      fflush(stdout);
    }
  }

  CtorTester(CtorTester &&Arg) : Value(new int(-1)) {
    dump("Operator Move Constructor");
    Arg.dump("    From");
    LLVM_DEBUG(llvm::errs() << "\n");
    assert(Value);
    assert(Arg.Value);
    // If Arg is not ignorable, it will be now and we will not be.
    if (!Arg.isIgnorableTester()) {
      EXPECT_TRUE(ConstructedTesters->insert(this));
      EXPECT_EQ(1u, ConstructedTesters->erase(&Arg));
    }
    std::swap(Value, Arg.Value);
    LLVM_DEBUG(fflush(stdout));
  }

  CtorTester &operator=(const CtorTester &Arg) {
    dump("Operator Copy Assignment");
    Arg.dump("    From");
    LLVM_DEBUG(llvm::errs() << "\n");
    assert(Value);
    assert(Arg.Value);

    // If arg is not an ignorable tester, but we are an ignorable tester, we
    // need to be inserted into the constructed testers set.
    if (!Arg.isIgnorableTester() && isIgnorableTester()) {
      EXPECT_TRUE(ConstructedTesters->insert(this));
    }
    *Value.get() = Arg.getValue();
    fflush(stdout);
    return *this;
  }

  CtorTester &operator=(CtorTester &&Arg) {
    dump("Operator Move Assignment");
    Arg.dump("    From");
    LLVM_DEBUG(llvm::errs() << "\n");
    assert(Value);
    assert(Arg.Value);
    if (!Arg.isIgnorableTester() && isIgnorableTester()) {
      EXPECT_EQ(1u, ConstructedTesters->erase(&Arg));
      EXPECT_TRUE(ConstructedTesters->insert(this));
    } else if (Arg.isIgnorableTester() && !isIgnorableTester()) {
      EXPECT_EQ(1u, ConstructedTesters->erase(this));
      EXPECT_TRUE(ConstructedTesters->insert(&Arg));
    }

    std::swap(Value, Arg.Value);
    LLVM_DEBUG(fflush(stdout));
    return *this;
  }

  ~CtorTester() {
    bool IsIgnorable = isIgnorableTester();
    dump("Destroying");
    LLVM_DEBUG(llvm::errs() << "\n");
    delete Value.get();
    Value = nullptr;
    fflush(stdout);
    if (ConstructedTesters->isClearing() || IsIgnorable)
      return;
    EXPECT_EQ(1u, ConstructedTesters->erase(this));
  }

  operator uint32_t() const { return *Value.get(); }

  int getValue() const { return *Value.get(); }

  bool operator==(const CtorTester &RHS) const {
    return *Value.get() == *RHS.Value.get();
  }

  bool isIgnorableTester() const {
    return *Value.get() >= -3 && *Value.get() < 0;
  }

private:
  void dump(StringRef Name) const {
    std::string Addr = "0x";
    Addr += llvm::utohexstr(uintptr_t(this));
    std::string ValueAddr = "0x";
    ValueAddr += llvm::utohexstr(uintptr_t(Value.get()));
    LLVM_DEBUG(llvm::errs() << Name << " <Tester Addr:" << Addr
               << " ValueAddr:" << ValueAddr << " Value:" << *Value.get()
               << ">");
  }
};

void CtorTesterSet::dumpLiveTesters() const {
  for (auto *Tester : Constructed) {
    if (Tester->isIgnorableTester())
      continue;
    llvm::SmallString<64> Hex;
    std::string Addr = llvm::utohexstr(uintptr_t(Tester));
    llvm::errs() << "<Tester Addr:" << Addr << " Value:" << Tester->getValue()
                 << ">\n";
  }
}

void CtorTesterSet::verifyTesters() const {
  for (auto *Tester : Constructed)
    EXPECT_TRUE(Tester->Value.isNonNull());
}

bool CtorTesterSet::hasLiveTesters() const {
  return std::any_of(Constructed.begin(), Constructed.end(),
                     [](CtorTester *T) -> bool {
                       assert(T);
                       return !T->isIgnorableTester();
                     });
}

bool CtorTesterSet::numLiveTesters() const {
  return llvm::count_if(Constructed, [](CtorTester *T) -> bool {
    assert(T);
    return !T->isIgnorableTester();
  });
}

void CtorTesterSet::clearTesters() {
  IsClearing = true;
  Constructed.clear();
  IsClearing = false;
}

bool CtorTesterSet::isLive(CtorTester *T) const {
  // if (IsClearing)
  //  return true;
  return Constructed.count(T);
}

bool CtorTesterSet::insert(CtorTester *T) {
  return Constructed.insert(T).second;
}

unsigned CtorTesterSet::erase(CtorTester *T) { return Constructed.erase(T); }

void CtorTesterSet::finalize() {
  IsClearing = true;
  Constructed.clear();
}

struct CtorTesterMapInfo {
  static inline CtorTester getEmptyKey() {
    return CtorTester(CtorTester::EmptyTester::Kind);
  }
  static inline CtorTester getTombstoneKey() {
    return CtorTester(CtorTester::TombstoneTester::Kind);
  }
  static unsigned getHashValue(const CtorTester &Val) {
    return Val.getValue() * 37u;
  }
  static bool isEqual(const CtorTester &LHS, const CtorTester &RHS) {
    return LHS == RHS;
  }
};

CtorTester getTestKey(int i, CtorTester *) { return CtorTester(i); }
CtorTester getTestValue(int i, CtorTester *) { return CtorTester(42 + i); }

} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                                   Tests
//===----------------------------------------------------------------------===//

namespace {

// Test fixture, with helper functions implemented by forwarding to global
// function overloads selected by component types of the type parameter. This
// allows all of the map implementations to be tested with shared
// implementations of helper routines.
template <typename T> class BlotMapVectorTest : public ::testing::Test {
protected:
  T Map;

  llvm::Optional<unsigned> NumExpectedLiveTesters;

  static typename T::key_type *const dummy_key_ptr;
  static typename T::mapped_type *const dummy_value_ptr;

public:
  void SetUp() override {
    ConstructedTesters->init();
    EXPECT_TRUE(!ConstructedTesters->hasLiveTesters());
  }

  ~BlotMapVectorTest() override {
    ConstructedTesters->verifyTesters();
    LLVM_DEBUG(llvm::errs() << "Destroying Fixture\n");
    ConstructedTesters->finalize();
  }

  void TearDown() override {
    if (std::is_same<T, CtorTester>::value)
      EXPECT_EQ(ConstructedTesters->numLiveTesters(), *NumExpectedLiveTesters);
  }

protected:
  typename T::key_type getKey(int i = 4) {
    return getTestKey(i, dummy_key_ptr);
  }
  typename T::mapped_type getValue(int i = 5) {
    return getTestValue(i, dummy_value_ptr);
  }
};

template <typename T>
typename T::key_type *const BlotMapVectorTest<T>::dummy_key_ptr = nullptr;
template <typename T>
typename T::mapped_type *const BlotMapVectorTest<T>::dummy_value_ptr = nullptr;

// Register these types for testing.
typedef ::testing::Types<
    BlotMapVector<uint32_t, uint32_t>, BlotMapVector<uint32_t *, uint32_t *>,
    BlotMapVector<CtorTester, CtorTester,
                  llvm::DenseMap<CtorTester, size_t, CtorTesterMapInfo>>,
    SmallBlotMapVector<uint32_t, uint32_t, 4>,
    SmallBlotMapVector<uint32_t *, uint32_t *, 4>,
    SmallBlotMapVector<
        CtorTester, CtorTester, 4,
        llvm::SmallDenseMap<CtorTester, unsigned, 4, CtorTesterMapInfo>>>
    BlotMapVectorTestTypes;
TYPED_TEST_SUITE(BlotMapVectorTest, BlotMapVectorTestTypes, );

// Empty map tests
TYPED_TEST(BlotMapVectorTest, EmptyIntMapTest) {
  // Size tests
  EXPECT_EQ(0u, this->Map.size());
  EXPECT_TRUE(this->Map.empty());

  // Iterator tests
  EXPECT_TRUE(this->Map.begin() == this->Map.end());

  // Lookup tests
  EXPECT_FALSE(this->Map.count(this->getKey()));
  EXPECT_TRUE(this->Map.find(this->getKey()) == this->Map.end());
  EXPECT_EQ(typename TypeParam::mapped_type(),
            this->Map.lookup(this->getKey()));
}

// Constant map tests
TYPED_TEST(BlotMapVectorTest, ConstEmptyMapTest) {
  const TypeParam &ConstMap = this->Map;
  EXPECT_EQ(0u, ConstMap.size());
  EXPECT_TRUE(ConstMap.empty());
  EXPECT_TRUE(ConstMap.begin() == ConstMap.end());
}

// A map with a single entry
TYPED_TEST(BlotMapVectorTest, SingleEntryMapTest) {
  this->Map[this->getKey()] = this->getValue();

  ConstructedTesters->verifyTesters();
  // Size tests
  EXPECT_EQ(1u, this->Map.size());
  EXPECT_FALSE(this->Map.begin() == this->Map.end());
  EXPECT_FALSE(this->Map.empty());

  // Iterator tests
  typename TypeParam::iterator it = this->Map.begin();
  EXPECT_EQ(this->getKey(), (*it)->first);
  EXPECT_EQ(this->getValue(), (*it)->second);
  ++it;
  EXPECT_TRUE(it == this->Map.end());

  // Lookup tests
  EXPECT_TRUE(this->Map.count(this->getKey()));
  EXPECT_TRUE(this->Map.find(this->getKey()) == this->Map.begin());
  EXPECT_EQ(this->getValue(), this->Map.lookup(this->getKey()));
  EXPECT_EQ(this->getValue(), this->Map[this->getKey()]);

  this->NumExpectedLiveTesters = 1;
}

// Test clear() method
TYPED_TEST(BlotMapVectorTest, ClearTest) {
  this->Map[this->getKey()] = this->getValue();
  this->Map.clear();

  EXPECT_EQ(0u, this->Map.size());
  EXPECT_TRUE(this->Map.empty());
  EXPECT_TRUE(this->Map.begin() == this->Map.end());
  this->NumExpectedLiveTesters = 0;
}

// Test erase(iterator) method
TYPED_TEST(BlotMapVectorTest, EraseTest) {
  this->Map[this->getKey()] = this->getValue();
  this->Map.erase(this->Map.begin());

  EXPECT_EQ(0u, this->Map.size());
  EXPECT_TRUE(this->Map.empty());
  EXPECT_TRUE(this->Map.begin() != this->Map.end());
  EXPECT_EQ(std::next(this->Map.begin()), this->Map.end());

  this->NumExpectedLiveTesters = 0;
}

// Test erase(value) method
TYPED_TEST(BlotMapVectorTest, EraseTest2) {
  this->Map[this->getKey()] = this->getValue();
  EXPECT_TRUE(this->Map.erase(this->getKey()));

  EXPECT_EQ(0u, this->Map.size());
  EXPECT_TRUE(this->Map.empty());
  EXPECT_TRUE(this->Map.begin() != this->Map.end());
  EXPECT_EQ(std::next(this->Map.begin()), this->Map.end());
  this->NumExpectedLiveTesters = 0;
}

// Test insert() method
TYPED_TEST(BlotMapVectorTest, InsertTest) {
  this->Map.insert(std::make_pair(this->getKey(), this->getValue()));
  EXPECT_EQ(1u, this->Map.size());
  EXPECT_EQ(this->getValue(), this->Map[this->getKey()]);
  EXPECT_EQ(1u, this->Map.size());
  this->NumExpectedLiveTesters = 1;
}

// Test try_emplace() method
TYPED_TEST(BlotMapVectorTest, TryEmplaceTest) {
  this->Map.try_emplace(this->getKey(), this->getValue());
  EXPECT_EQ(1u, this->Map.size());
  EXPECT_EQ(this->getValue(), this->Map[this->getKey()]);
  EXPECT_EQ(1u, this->Map.size());
  this->Map.try_emplace(this->getKey(), this->getValue());
  EXPECT_EQ(1u, this->Map.size());
  EXPECT_EQ(this->getValue(), this->Map[this->getKey()]);
  EXPECT_EQ(1u, this->Map.size());
  this->NumExpectedLiveTesters = 1;
}

// Test copy constructor method
TYPED_TEST(BlotMapVectorTest, CopyConstructorTest) {
  this->Map[this->getKey()] = this->getValue();
  TypeParam copyMap(this->Map);

  EXPECT_EQ(1u, copyMap.size());
  EXPECT_EQ(this->getValue(), copyMap[this->getKey()]);
  EXPECT_EQ(1u, copyMap.size());
  this->NumExpectedLiveTesters = 1;
}

// Test copy constructor method where SmallBlotMapVector isn't small.
TYPED_TEST(BlotMapVectorTest, CopyConstructorNotSmallTest) {
  for (int Key = 0; Key < 5; ++Key)
    this->Map[this->getKey(Key)] = this->getValue(Key);
  TypeParam copyMap(this->Map);

  EXPECT_EQ(5u, copyMap.size());
  for (int Key = 0; Key < 5; ++Key)
    EXPECT_EQ(this->getValue(Key), copyMap[this->getKey(Key)]);
  this->NumExpectedLiveTesters = 10;
}

// Test copying from a default-constructed map.
TYPED_TEST(BlotMapVectorTest, CopyConstructorFromDefaultTest) {
  TypeParam copyMap(this->Map);

  EXPECT_TRUE(copyMap.empty());
  this->NumExpectedLiveTesters = 0;
}

// Test copying from an empty map where SmallBlotMapVector isn't small.
TYPED_TEST(BlotMapVectorTest, CopyConstructorFromEmptyTest) {
  for (int Key = 0; Key < 5; ++Key)
    this->Map[this->getKey(Key)] = this->getValue(Key);
  this->Map.clear();
  TypeParam copyMap(this->Map);

  EXPECT_TRUE(copyMap.empty());
  this->NumExpectedLiveTesters = 0;
}

// Test assignment operator method
TYPED_TEST(BlotMapVectorTest, AssignmentTest) {
  this->Map[this->getKey()] = this->getValue();
  TypeParam copyMap = this->Map;

  EXPECT_EQ(1u, copyMap.size());
  EXPECT_EQ(this->getValue(), copyMap[this->getKey()]);

  // test self-assignment.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-assign-overloaded"
  copyMap = copyMap;
#pragma clang diagnostic pop
#else
  copyMap = copyMap;
#endif
  EXPECT_EQ(1u, copyMap.size());
  EXPECT_EQ(this->getValue(), copyMap[this->getKey()]);
  this->NumExpectedLiveTesters = 2;
}

// A more complex iteration test
TYPED_TEST(BlotMapVectorTest, IterationTest) {
  bool visited[100];
  std::map<typename TypeParam::key_type, unsigned> visitedIndex;

  // Insert 100 numbers into the map
  for (int i = 0; i < 100; ++i) {
    visited[i] = false;
    visitedIndex[this->getKey(i)] = i;

    this->Map[this->getKey(i)] = this->getValue(i);
  }

  // Iterate over all numbers and mark each one found.
  for (typename TypeParam::iterator it = this->Map.begin();
       it != this->Map.end(); ++it)
    visited[visitedIndex[(*it)->first]] = true;

  // Ensure every number was visited.
  for (int i = 0; i < 100; ++i)
    ASSERT_TRUE(visited[i]) << "Entry #" << i << " was never visited";
  this->NumExpectedLiveTesters = 100;
}

// const_iterator test
TYPED_TEST(BlotMapVectorTest, ConstIteratorTest) {
  // Check conversion from iterator to const_iterator.
  typename TypeParam::iterator it = this->Map.begin();
  typename TypeParam::const_iterator cit(it);
  EXPECT_TRUE(it == cit);

  // Check copying of const_iterators.
  typename TypeParam::const_iterator cit2(cit);
  EXPECT_TRUE(cit == cit2);
}

// Make sure BlotMapVector works with StringRef keys.
TEST(BlotMapVectorCustomTest, StringRefTest) {
  BlotMapVector<StringRef, int> M;

  M["a"] = 1;
  M["b"] = 2;
  M["c"] = 3;

  EXPECT_EQ(3u, M.size());
  EXPECT_EQ(1, M.lookup("a"));
  EXPECT_EQ(2, M.lookup("b"));
  EXPECT_EQ(3, M.lookup("c"));

  EXPECT_EQ(0, M.lookup("q"));

  // Test the empty string, spelled various ways.
  EXPECT_EQ(0, M.lookup(""));
  EXPECT_EQ(0, M.lookup(StringRef()));
  EXPECT_EQ(0, M.lookup(StringRef("a", 0)));
  M[""] = 42;
  EXPECT_EQ(42, M.lookup(""));
  EXPECT_EQ(42, M.lookup(StringRef()));
  EXPECT_EQ(42, M.lookup(StringRef("a", 0)));
}

// Key traits that allows lookup with either an unsigned or char* key;
// In the latter case, "a" == 0, "b" == 1 and so on.
struct TestBlotMapVectorInfo {
  static inline unsigned getEmptyKey() { return ~0; }
  static inline unsigned getTombstoneKey() { return ~0U - 1; }
  static unsigned getHashValue(const unsigned &Val) { return Val * 37U; }
  static unsigned getHashValue(const char *Val) {
    return (unsigned)(Val[0] - 'a') * 37U;
  }
  static bool isEqual(const unsigned &LHS, const unsigned &RHS) {
    return LHS == RHS;
  }
  static bool isEqual(const char *LHS, const unsigned &RHS) {
    return (unsigned)(LHS[0] - 'a') == RHS;
  }
};

// find_as() tests
TEST(BlotMapVectorCustomTest, FindAsTest) {
  BlotMapVector<unsigned, unsigned,
                llvm::DenseMap<unsigned, unsigned, TestBlotMapVectorInfo>> map;
  map[0] = 1;
  map[1] = 2;
  map[2] = 3;

  // Size tests
  EXPECT_EQ(3u, map.size());

  // Normal lookup tests
  EXPECT_EQ(1u, map.count(1));
  EXPECT_EQ(1u, map.find(0)->value().second);
  EXPECT_EQ(2u, map.find(1)->value().second);
  EXPECT_EQ(3u, map.find(2)->value().second);
  EXPECT_TRUE(map.find(3) == map.end());
}

struct ContiguousBlotMapVectorInfo {
  static inline unsigned getEmptyKey() { return ~0; }
  static inline unsigned getTombstoneKey() { return ~0U - 1; }
  static unsigned getHashValue(const unsigned &Val) { return Val; }
  static bool isEqual(const unsigned &LHS, const unsigned &RHS) {
    return LHS == RHS;
  }
};

// Test that filling a small dense map with exactly the number of elements in
// the map grows to have enough space for an empty bucket.
TEST(BlotMapVectorCustomTest, SmallBlotMapVectorGrowTest) {
  SmallBlotMapVector<unsigned, unsigned, 32,
                     llvm::SmallDenseMap<unsigned, unsigned, 32,
                                         ContiguousBlotMapVectorInfo>> map;
  // Add some number of elements, then delete a few to leave us some tombstones.
  // If we just filled the map with 32 elements we'd grow because of not enough
  // tombstones which masks the issue here.
  for (unsigned i = 0; i < 20; ++i)
    map[i] = i + 1;
  for (unsigned i = 0; i < 10; ++i)
    EXPECT_TRUE(map.erase(i));
  for (unsigned i = 20; i < 32; ++i)
    map[i] = i + 1;
  for (unsigned i = 0; i < 10; ++i)
    EXPECT_FALSE(map.erase(i));

  // Size tests
  EXPECT_EQ(22u, map.size());

  // Try to find an element which doesn't exist.  There was a bug in
  // SmallBlotMapVector which led to a map with num elements == small capacity
  // not
  // having an empty bucket any more.  Finding an element not in the map would
  // therefore never terminate.
  EXPECT_TRUE(map.find(32) == map.end());
}

} // end anonymous namespace

#ifndef TEST_INTEROP_CXX_STDLIB_INPUTS_CUSTOM_SEQUENCE_H
#define TEST_INTEROP_CXX_STDLIB_INPUTS_CUSTOM_SEQUENCE_H

#include "custom-iterator.h"
#include <iterator>

struct SimpleSequence {
  ConstIterator begin() const { return ConstIterator(1); }
  ConstIterator end() const { return ConstIterator(5); }
};

struct SimpleSequenceWithOutOfLineEqualEqual {
  ConstIteratorOutOfLineEq begin() const { return ConstIteratorOutOfLineEq(1); }
  ConstIteratorOutOfLineEq end() const { return ConstIteratorOutOfLineEq(5); }
};

static int copiesCount = 0;

struct SimpleCopyAwareSequence {
  ConstIterator begin() const { return ConstIterator(1); }
  ConstIterator end() const { return ConstIterator(5); }

  SimpleCopyAwareSequence() {}
  SimpleCopyAwareSequence(const SimpleCopyAwareSequence &other) { copiesCount++; }
};

struct SimpleArrayWrapper {
private:
  int a[5] = {10, 20, 30, 40, 50};

public:
  const int *begin() const __attribute__((returns_nonnull)) { return &a[0]; }
  const int *end() const __attribute__((returns_nonnull)) { return &a[5]; }
};

struct SimpleArrayWrapperNullableIterators {
private:
  int a[5] = {10, 20, 30, 40, 50};

public:
  const int *begin() const { return &a[0]; }
  const int *end() const { return &a[5]; }
};

struct SimpleEmptySequence {
  const int *begin() const { return nullptr; }
  const int *end() const { return nullptr; }
};

struct HasMutatingBeginEnd {
  ConstIterator begin() { return ConstIterator(1); }
  ConstIterator end() { return ConstIterator(5); }
};

// TODO: this should conform to CxxSequence.
struct __attribute__((swift_attr("import_reference"),
                      swift_attr("retain:immortal"),
                      swift_attr("release:immortal"))) ImmortalSequence {
  ConstIterator begin() { return ConstIterator(1); }
  ConstIterator end() { return ConstIterator(5); }
};

// MARK: Types that are not actually sequences

struct HasNoBeginMethod {
  ConstIterator end() const { return ConstIterator(1); }
};

struct HasNoEndMethod {
  ConstIterator begin() const { return ConstIterator(1); }
};

struct HasBeginEndTypeMismatch {
  ConstIterator begin() const { return ConstIterator(1); }
  ConstIteratorOutOfLineEq end() const { return ConstIteratorOutOfLineEq(3); }
};

struct HasBeginEndReturnNonIterators {
  struct NotIterator {};

  NotIterator begin() const { return NotIterator(); }
  NotIterator end() const { return NotIterator(); }
};

// TODO: this should not be conformed to CxxSequence, because
//  `const ConstIterator &` is imported as `UnsafePointer<ConstIterator>`, and
//  calling `successor()` is not actually going to call
//  `ConstIterator::operator++()`. It will increment the address instead.
struct HasBeginEndReturnRef {
private:
  ConstIterator b = ConstIterator(1);
  ConstIterator e = ConstIterator(5);

public:
  const ConstIterator &begin() const { return b; }
  const ConstIterator &end() const { return e; }
};

template <typename A> struct NoDefinition;

template <typename A, typename NoDef = NoDefinition<A>>
struct HasTemplatedIterator {
  typedef NoDef* iterator; // OpaquePointer

  iterator begin() const;
  iterator end() const;
};

typedef HasTemplatedIterator<int> HasUninstantiatableIterator;

struct HasInheritedConstIterator {
private:
  InheritedConstIterator b = InheritedConstIterator(1);
  InheritedConstIterator e = InheritedConstIterator(6);

public:
  InheritedConstIterator begin() const { return b; }
  InheritedConstIterator end() const { return e; }
};

template <typename T>
struct HasInheritedTemplatedConstIterator {
public:
  typedef InheritedTemplatedConstIterator<int> iterator;

private:
  iterator b = iterator(1);
  iterator e = iterator(7);

public:
  iterator begin() const { return b; }
  iterator end() const { return e; }
};

typedef HasInheritedTemplatedConstIterator<int>
    HasInheritedTemplatedConstIteratorInt;

struct HasInputOutputConstIterator {
  typedef InputOutputConstIterator iterator;

  mutable int x[5] = {5, 4, 3, 2, 1};

  iterator begin() const { return iterator(x); }
  iterator end() const { return iterator(x + 5); }
};

#endif // TEST_INTEROP_CXX_STDLIB_INPUTS_CUSTOM_SEQUENCE_H

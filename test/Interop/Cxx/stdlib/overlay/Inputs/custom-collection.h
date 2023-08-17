#ifndef TEST_INTEROP_CXX_STDLIB_INPUTS_CUSTOM_COLLECTION_H
#define TEST_INTEROP_CXX_STDLIB_INPUTS_CUSTOM_COLLECTION_H

#include "custom-iterator.h"
#include <iterator>

struct SimpleCollectionNoSubscript {
private:
  int x[5] = {1, 2, 3, 4, 5};

public:
  using iterator = ConstRACIterator;

  iterator begin() const { return iterator(*x); }
  iterator end() const { return iterator(*x + 5); }
};

struct SimpleCollectionReadOnly {
private:
  int x[5] = {1, 2, 3, 4, 5};

public:
  using iterator = ConstRACIteratorRefPlusEq;

  iterator begin() const { return iterator(*x); }
  iterator end() const { return iterator(*x + 5); }

  const int& operator[](int index) const { return x[index]; }
};

template <typename T>
struct HasInheritedTemplatedConstRACIterator {
public:
  typedef InheritedTemplatedConstRACIterator<int> iterator;

private:
  iterator b = iterator(1);
  iterator e = iterator(6);

public:
  iterator begin() const { return b; }
  iterator end() const { return e; }
};

typedef HasInheritedTemplatedConstRACIterator<int>
    HasInheritedTemplatedConstRACIteratorInt;

template <typename T>
struct HasInheritedTemplatedConstRACIteratorOutOfLineOps {
public:
  typedef InheritedTemplatedConstRACIteratorOutOfLineOps<int> iterator;

private:
  iterator b = iterator(1);
  iterator e = iterator(4);

public:
  iterator begin() const { return b; }
  iterator end() const { return e; }
};

typedef HasInheritedTemplatedConstRACIteratorOutOfLineOps<int>
    HasInheritedTemplatedConstRACIteratorOutOfLineOpsInt;

#endif // TEST_INTEROP_CXX_STDLIB_INPUTS_CUSTOM_COLLECTION_H

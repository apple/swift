#ifndef TEST_INTEROP_CXX_CLASS_AMBIGUOUS_METHOD_METHODS_H
#define TEST_INTEROP_CXX_CLASS_AMBIGUOUS_METHOD_METHODS_H

struct HasAmbiguousMethods {

  // One input (const first)
  int increment(int a) const {
    return a + 1;
  }

  int increment(int a) {
    ++mutableMethodsCalledCount;
    return a + 1;
  }

  // Multiple input with out param
  void increment(int a, int b, int &c) {
    ++mutableMethodsCalledCount;
    c = a + b;
  }

  void increment(int a, int b, int &c) const {
    c = a + b;
  }

  // Multiple input with inout param
  void increment(int &a, int b) {
    ++mutableMethodsCalledCount;
    a += b;
  }

  void increment(int &a, int b) const {
    a += b;
  }

  // No input with output (const first)
  int numberOfMutableMethodsCalled() const { return mutableMethodsCalledCount; }
  int numberOfMutableMethodsCalled() { return ++mutableMethodsCalledCount; }

private:
  int mutableMethodsCalledCount = 0;
};

struct HasAmbiguousMethods2 {
  int increment(int a) const {
    return a + 1;
  }
};

struct Unsafe {
  int *ptr;
};

struct HasAmbiguousUnsafeMethods {
  HasAmbiguousUnsafeMethods(const HasAmbiguousUnsafeMethods&);
  Unsafe getUnsafe() const { return Unsafe(); }
  Unsafe getUnsafe() { return Unsafe(); }
};

#endif // TEST_INTEROP_CXX_CLASS_AMBIGUOUS_METHOD_METHODS_H

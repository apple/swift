#ifndef TEST_INTEROP_CXX_CLASS_METHOD_METHODS_H
#define TEST_INTEROP_CXX_CLASS_METHOD_METHODS_H

struct __attribute__((swift_attr("import_unsafe"))) NonTrivialInWrapper {
  int value;

  NonTrivialInWrapper(int value) : value(value) {}

  // explicit copy constructor is needed, as on Windows a destructor
  // still makes this a type that's passed in registers.
  NonTrivialInWrapper(const NonTrivialInWrapper &other) : value(other.value) {}
  ~NonTrivialInWrapper() { }
};

struct HasMethods {
  void nonConstMethod() { }
  void constMethod() const { }

  int nonConstPassThrough(int a) { return a; }
  int constPassThrough(int a) const { return a; }

  int nonConstSum(int a, int b) { return a + b; }
  int constSum(int a, int b) const { return a + b; }

  int nonConstSum(NonTrivialInWrapper a, NonTrivialInWrapper b) { return a.value + b.value; }
  int constSum(NonTrivialInWrapper a, NonTrivialInWrapper b) const { return a.value + b.value; }

  NonTrivialInWrapper nonConstSumAsWrapper(NonTrivialInWrapper a, NonTrivialInWrapper b) { return {a.value + b.value}; }
  NonTrivialInWrapper constSumAsWrapper(NonTrivialInWrapper a, NonTrivialInWrapper b) const { return {a.value + b.value}; }

  NonTrivialInWrapper nonConstPassThroughAsWrapper(int a) { return {a}; }
  NonTrivialInWrapper constPassThroughAsWrapper(int a) const { return {a}; }
};

struct ReferenceParams {
  int a;
  int b;
  ReferenceParams(const int &a, const int &b) : a(a), b(b) { }
  static void staticMethod(const int &a, const int &b) {
    ReferenceParams t{a, b};
  }
};

#endif // TEST_INTEROP_CXX_CLASS_METHOD_METHODS_H

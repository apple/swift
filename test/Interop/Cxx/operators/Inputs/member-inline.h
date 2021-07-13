#ifndef TEST_INTEROP_CXX_OPERATORS_INPUTS_MEMBER_INLINE_H
#define TEST_INTEROP_CXX_OPERATORS_INPUTS_MEMBER_INLINE_H

struct LoadableIntWrapper {
  int value;
  LoadableIntWrapper operator-(LoadableIntWrapper rhs) {
    return LoadableIntWrapper{.value = value - rhs.value};
  }

  int operator()() {
    return value;
  }
  int operator()(int x) {
    return value + x;
  }
  int operator()(int x, int y) {
    return value + x * y;
  }
};

struct AddressOnlyIntWrapper {
  int value;

  AddressOnlyIntWrapper(int value) : value(value) {}
  AddressOnlyIntWrapper(const AddressOnlyIntWrapper &other) : value(other.value) {}

  int operator()() {
    return value;
  }
  int operator()(int x) {
    return value + x;
  }
  int operator()(int x, int y) {
    return value + x * y;
  }
};

struct HasDeletedOperator {
  void operator!=(HasDeletedOperator) const = delete;
};

struct ReadWriteIntArray {
private:
  int values[5] = { 1, 2, 3, 4, 5 };

public:
  const int &operator[](int x) const {
    return values[x];
  }
  int &operator[](int x) {
    return values[x];
  }

  struct NestedIntArray {
  private:
    int values[5] = { 1, 2, 3, 4, 5 };

  public:
    const int &operator[](int x) const {
      return values[x];
    }
  };
};

struct ReadOnlyIntArray {
private:
  int values[5] = { 1, 2, 3, 4, 5 };

public:
  ReadOnlyIntArray(int first) {
    values[0] = first;
  }
  ReadOnlyIntArray(const ReadOnlyIntArray &other) {}

  const int &operator[](int x) const {
    return values[x];
  }
};

struct WriteOnlyIntArray {
private:
  int values[5] = { 1, 2, 3, 4, 5 };

public:
  int &operator[](int x) {
    return values[x];
  }
};

struct DifferentTypesArray {
private:
  int values[3] = { 1, 2, 3 };
  double doubleValues[2] = { 1.5, 2.5 };
  bool boolValues[2] = { true, false };

public:
  const int &operator[](int x) const {
    return values[x];
  }
  int &operator[](int x) {
    return values[x];
  }
  bool &operator[](bool x) {
    return boolValues[x];
  }
  const bool &operator[](bool x) const {
    return boolValues[x];
  }
  const double &operator[](double x) const {
    return doubleValues[x == 0.0];
  }
};

template<class T>
struct TemplatedArray {
  T ts[];

  T &operator[](int i) {
    return ts[i];
  }
  const T &operator[](int i) const {
    return ts[i];
  }
};
typedef TemplatedArray<double> TemplatedDoubleArray;

struct TemplatedSubscriptArray {
  int *ptr;

  template<class T>
  T &operator[](T i) {
    return ptr[i];
  }
  template<class T>
  const T &operator[](T i) const {
    return ptr[i];
  }
};

struct IntArrayByVal {
  // For testing purposes.
  void setValueAtIndex(int value, unsigned i) { values[i] = value; }
  int operator[](int x) const { return values[x]; }
private:
  int values[3] = { 1, 2, 3 };
};

struct NonTrivialIntArrayByVal {
  NonTrivialIntArrayByVal(int first) { values[0] = first; }
  NonTrivialIntArrayByVal(const NonTrivialIntArrayByVal &other) {
    for (int i = 0; i < 5; i++)
      values[i] = other.values[i];
  }

  int operator[](int x) const { return values[x]; }

  // For testing purposes.
  void setValueAtIndex(int value, unsigned i) { values[i] = value; }

private:
  int values[5] = { 1, 2, 3, 4, 5 };
};

struct DifferentTypesArrayByVal {
  int operator[](int x) { return values[x]; }
  bool operator[](bool x) { return boolValues[x]; }
  double operator[](double x) const { return doubleValues[x == 0.0]; }

private:
  int values[3] = { 1, 2, 3 };
  double doubleValues[2] = { 1.5, 2.5 };
  bool boolValues[2] = { true, false };
};

template<class T> struct TemplatedArrayByVal {
  T ts[];
  T operator[](int i) { return ts[i]; }
};
typedef TemplatedArrayByVal<double> TemplatedDoubleArrayByVal;

struct TemplatedSubscriptArrayByVal {
  int *ptr;
  template<class T> T operator[](T i) { return ptr[i]; }
};

struct NonTrivial {
  char *Str;
  long long a;
  short b;
  long long c;
  short d;
  long long e;
  int f;
  NonTrivial() {
    Str = (char*)"Non-Trivial";
    a = 1;
    b = 2;
    c = 3;
    d = 4;
    e = 5;
    f = 6;
  }
  ~NonTrivial() { Str = nullptr; }
};

struct NonTrivialArrayByVal {
  NonTrivial operator[](int x) { return S; }
private:
  NonTrivial S;
};

struct PtrByVal {
  int *operator[](int x) { return &a; }
private:
  int a = 64;
};

struct RefToPtr {
  RefToPtr() { b = &a; }
  int *&operator[](int x) { return b; }
private:
  int a = 64;
  int *b = nullptr;
};

struct PtrToPtr {
  PtrToPtr() { b = &a; }
  int **operator[](int x) { return &b; }
private:
  int a = 64;
  int *b = nullptr;
};

struct ConstOpPtrByVal {
  const int *operator[](int x) const { return &a; }
private:
  int a = 64;
};

struct ConstPtrByVal {
  const int *operator[](int x) { return &a; }
private:
  int a = 64;
};

#endif

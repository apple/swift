#ifndef TEST_INTEROP_CXX_OPERATORS_INPUTS_MEMBER_INLINE_H
#define TEST_INTEROP_CXX_OPERATORS_INPUTS_MEMBER_INLINE_H

struct LoadableIntWrapper {
  int value;
  LoadableIntWrapper operator-(LoadableIntWrapper rhs) {
    return LoadableIntWrapper{.value = value - rhs.value};
  }

  LoadableIntWrapper operator+=(LoadableIntWrapper rhs) {
    value += rhs.value;
    return *this;
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

  operator int() const { return value; }

  LoadableIntWrapper &operator++() {
    value++;
    return *this;
  }

  // Friend functions
  friend bool operator==(const LoadableIntWrapper &lhs,
                         const LoadableIntWrapper &rhs) {
    return lhs.value == rhs.value;
  }

  friend LoadableIntWrapper operator-(const LoadableIntWrapper& obj) {
    return LoadableIntWrapper{.value = -obj.value};
  }

  friend LoadableIntWrapper operator-=(LoadableIntWrapper& lhs, const LoadableIntWrapper& rhs) {
    lhs.value -= rhs.value;
    return lhs;
  }
};

namespace NS {
struct IntWrapperInNamespace {
  int value;
  friend bool operator==(const IntWrapperInNamespace &lhs,
                         const IntWrapperInNamespace &rhs) {
    return lhs.value == rhs.value;
  }
};
} // namespace NS

struct LoadableBoolWrapper {
  bool value;
  LoadableBoolWrapper operator!() {
    return LoadableBoolWrapper{.value = !value};
  }
  operator bool() const { return value; }
};

template<class T>
struct TemplatedWithFriendOperator {
  friend bool operator==(const TemplatedWithFriendOperator &lhs,
                         const TemplatedWithFriendOperator &rhs) {
    return true;
  }
};

using TemplatedWithFriendOperatorSpec = TemplatedWithFriendOperator<int>;

struct __attribute__((swift_attr("import_owned"))) AddressOnlyIntWrapper {
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

  AddressOnlyIntWrapper operator-(AddressOnlyIntWrapper rhs) const {
    return AddressOnlyIntWrapper(value - rhs.value);
  }
  AddressOnlyIntWrapper &operator++() {
    value++;
    return *this;
  }
  AddressOnlyIntWrapper operator++(int) {
    // This shouldn't be called, since we only support pre-increment operators.
    return AddressOnlyIntWrapper(-777);
  }
};

struct HasPostIncrementOperator {
  HasPostIncrementOperator operator++(int) {
    return HasPostIncrementOperator();
  }
};

struct HasPreIncrementOperatorWithAnotherReturnType {
  int value = 0;
  const int &operator++() { return ++value; }
};

struct HasPreIncrementOperatorWithVoidReturnType {
  int value = 0;
  void operator++() { ++value; }
};

struct __attribute__((swift_attr("import_reference"),
                      swift_attr("retain:immortal"),
                      swift_attr("release:immortal"))) ImmortalCounter {
  int value = 0;

  ImmortalCounter &operator++() {
    value++;
    return *this;
  }
};
static ImmortalCounter myCounter;

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

struct __attribute__((swift_attr("import_owned"))) ReadOnlyIntArray {
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

struct ReadOnlyRvalueParam {
private:
  int values[5] = {1, 2, 3, 4, 5};

public:
  const int &operator[](int &&x) const { return values[x]; }
};

struct ReadWriteRvalueParam {
private:
  int values[5] = {1, 2, 3, 4, 5};

public:
  const int &operator[](int &&x) const { return values[x]; }
  int &operator[](int&& x) { return values[x]; }
};

struct ReadWriteRvalueGetterParam {
private:
  int values[5] = {1, 2, 3, 4, 5};
  
public:
  const int &operator[](int &&x) const { return values[x]; }
  int &operator[](int x) { return values[x]; }
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

struct __attribute__((swift_attr("import_unsafe"))) TemplatedSubscriptArray {
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

struct __attribute__((swift_attr("import_owned"))) NonTrivialIntArrayByVal {
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

template <class T>
struct TemplatedByVal {
  T val;
  TemplatedByVal<T> operator+(TemplatedByVal other) {
    return TemplatedByVal{.val = val + other.val};
  }
};

struct __attribute__((swift_attr("import_unsafe")))
TemplatedOperatorArrayByVal {
  int *ptr;
  template<class T> T operator[](T i) { return ptr[i]; }
  template <class T>
  T *operator+(T i) {
    return ptr + i;
  }
};

struct __attribute__((swift_attr("import_owned"))) NonTrivial {
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

struct __attribute__((swift_attr("import_unsafe"))) RefToPtr {
  RefToPtr() { b = &a; }
  int *&operator[](int x) { return b; }
private:
  int a = 64;
  int *b = nullptr;
};

struct __attribute__((swift_attr("import_unsafe"))) PtrToPtr {
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

struct DerivedFromAddressOnlyIntWrapper : AddressOnlyIntWrapper {
  DerivedFromAddressOnlyIntWrapper(int value) : AddressOnlyIntWrapper(value) { }
};

struct DerivedFromReadWriteIntArray : ReadWriteIntArray {};

struct DerivedFromNonTrivialArrayByVal : NonTrivialArrayByVal {};

struct SubscriptUnnamedParameter {
  int operator[](int) const { return 123; }
};

struct SubscriptUnnamedParameterReadWrite {
  int value = 0;
  const int &operator[](int) const { return value; }
  int &operator[](int) { return value; }
};

struct Iterator {
private:
  int value = 123;
public:
  int &operator*() { return value; }
};

struct ConstIterator {
private:
  int value = 234;
public:
  const int &operator*() const { return value; }
};

struct ConstIteratorByVal {
private:
  int value = 456;
public:
  int operator*() const { return value; }
};

struct AmbiguousOperatorStar {
private:
  int value = 567;
public:
  int &operator*() { return value; }
  const int &operator*() const { return value; }
};

struct AmbiguousOperatorStar2 {
private:
  int value = 678;
public:
  int &operator*() & { return value; }
  const int &operator*() const & { return value; }
  const int &&operator*() const && { return static_cast<const int &&>(value); }
};

struct DerivedFromConstIterator : public ConstIterator {};

struct DerivedFromConstIteratorPrivately : private ConstIterator {};

class SubscriptSetterConst {
public:
  using T = int;

  SubscriptSetterConst() : p(new T[10]) {}

  T& operator[](int i) const {
    return p[i];
  }
private:
  T *p;
};

struct DerivedFromConstIteratorPrivatelyWithUsingDecl : private ConstIterator {
  using ConstIterator::operator*;
};

struct DerivedFromAmbiguousOperatorStarPrivatelyWithUsingDecl
    : private AmbiguousOperatorStar {
  using AmbiguousOperatorStar::operator*;
};

struct DerivedFromLoadableIntWrapperWithUsingDecl : private LoadableIntWrapper {
  using LoadableIntWrapper::operator-;
  using LoadableIntWrapper::operator+=;

  int getValue() const {
    return value;
  }
  void setValue(int v) {
    this->value = v;
  }
};

struct HasOperatorCallWithDefaultArg {
  int value;
  int operator()(int x = 0) const { return value + x; }
};

class HasStaticOperatorCallBase {
public:
  static int operator()(int x) { return x + 42; }
};

class HasStaticOperatorCallDerived : public HasStaticOperatorCallBase {};

class HasStaticOperatorCallWithConstOperator {
public:
  inline int operator()(int x, int y) const { return x + y; }
  static int operator()(int x) {
        return x - 1;
   }
};

struct ClassWithOperatorStarAvailable {
  int value;

public:
  int &operator*() { return value; }
};

struct DerivedClassWithOperatorStarAvailable : ClassWithOperatorStarAvailable {
};

struct ClassWithOperatorStarUnavailable {
  int value;

public:
  int &operator*() __attribute__((availability(swift, unavailable))) {
    return value;
  }
};

struct DerivedClassWithOperatorStarUnavailable
    : ClassWithOperatorStarUnavailable {};

struct ClassWithSuccessorAvailable {
  int value;

public:
  ClassWithSuccessorAvailable &operator++() {
    value++;
    return *this;
  }
};

struct ClassWithSuccessorUnavailable {
  int value;

public:
  ClassWithSuccessorUnavailable &operator++()
      __attribute__((availability(swift, unavailable))) {
    value++;
    return *this;
  }
};

#endif

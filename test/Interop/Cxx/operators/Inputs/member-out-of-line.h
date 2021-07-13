#ifndef TEST_INTEROP_CXX_OPERATORS_INPUTS_MEMBER_OUT_OF_LINE_H
#define TEST_INTEROP_CXX_OPERATORS_INPUTS_MEMBER_OUT_OF_LINE_H

struct LoadableIntWrapper {
  int value;
  LoadableIntWrapper operator+(LoadableIntWrapper rhs) const;
  int operator()() const;
  int operator()(int x) const;
  int operator()(int x, int y) const;
};

struct AddressOnlyIntWrapper {
  int value;

  AddressOnlyIntWrapper(int value) : value(value) {}
  AddressOnlyIntWrapper(const AddressOnlyIntWrapper &other) : value(other.value) {}

  int operator()() const;
  int operator()(int x) const;
  int operator()(int x, int y) const;
};

struct ReadWriteIntArray {
private:
  int values[5] = { 1, 2, 3, 4, 5 };

public:
  const int &operator[](int x) const;
  int &operator[](int x);
};

struct NonTrivialIntArrayByVal {
  NonTrivialIntArrayByVal(int first) { values[0] = first; }
  NonTrivialIntArrayByVal(const NonTrivialIntArrayByVal &other) {
    for (int i = 0; i < 5; i++)
      values[i] = other.values[i];
  }
  int operator[](int x);

  // For testing purposes.
  void setValueAtIndex(int value, unsigned i) { values[i] = value; }

private:
  int values[5] = { 1, 2, 3, 4, 5 };
};

#endif
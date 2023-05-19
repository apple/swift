#ifndef TEST_INTEROP_CXX_IMPLEMENTATION_ONLY_IMPORTS_INPUTS_HELPER_H
#define TEST_INTEROP_CXX_IMPLEMENTATION_ONLY_IMPORTS_INPUTS_HELPER_H

inline int getFortyTwo() { return 42; }

class MagicWrapper {
public:
  int _number;
  MagicWrapper(){_number = 2;};
  MagicWrapper(int number) : _number(number){};
  MagicWrapper operator - (MagicWrapper other) {
      return MagicWrapper{_number - other._number};
  }

  int baseMethod() const { return 42; }
};

inline MagicWrapper operator + (MagicWrapper lhs, MagicWrapper rhs) {
  return MagicWrapper{lhs._number + rhs._number};
}

class MagicWrapperDerived: public MagicWrapper {
public:
  MagicWrapperDerived() { };
};

#endif // TEST_INTEROP_CXX_IMPLEMENTATION_ONLY_IMPORTS_INPUTS_HELPER_H

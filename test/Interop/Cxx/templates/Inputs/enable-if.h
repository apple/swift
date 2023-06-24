#ifndef TEST_INTEROP_CXX_TEMPLATES_INPUTS_ENABLE_IF_H
#define TEST_INTEROP_CXX_TEMPLATES_INPUTS_ENABLE_IF_H

template <bool B, class T = void>
struct enable_if {};

template <class T>
struct enable_if<true, T> {
  typedef T type;
};

template <class T>
struct is_bool {
  static const bool value = false;
};

template <>
struct is_bool<bool> {
  static const bool value = true;
};

struct HasMethodWithEnableIf {
  template <typename T>
  typename enable_if<is_bool<T>::value, bool>::type onlyEnabledForBool(T t) const {
    return !t;
  }
};

#endif // TEST_INTEROP_CXX_TEMPLATES_INPUTS_ENABLE_IF_H

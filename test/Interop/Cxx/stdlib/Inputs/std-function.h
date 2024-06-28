#ifndef TEST_INTEROP_CXX_STDLIB_INPUTS_STD_FUNCTION_H
#define TEST_INTEROP_CXX_STDLIB_INPUTS_STD_FUNCTION_H

#include <functional>
#include <string>

using FunctionIntToInt = std::function<int(int)>;
using FunctionStringToString = std::function<std::string(const std::string&)>;

inline FunctionIntToInt getIdentityFunction() {
  return [](int x) { return x; };
}

inline bool isEmptyFunction(FunctionIntToInt f) { return !(bool)f; }

inline int invokeFunction(FunctionIntToInt f, int x) { return f(x); }

std::string invokeFunctionTwice(FunctionStringToString f, std::string s) {
  return f(f(s));
}

#endif // TEST_INTEROP_CXX_STDLIB_INPUTS_STD_FUNCTION_H

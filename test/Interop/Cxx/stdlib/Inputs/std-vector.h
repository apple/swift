#ifndef TEST_INTEROP_CXX_STDLIB_INPUTS_STD_VECTOR_H
#define TEST_INTEROP_CXX_STDLIB_INPUTS_STD_VECTOR_H

#include <vector>
#include <string>

using Vector = std::vector<int>;
using VectorOfString = std::vector<std::string>;

inline Vector initVector() { return {}; }

inline std::string takesVectorOfString(const VectorOfString &v) {
  return v.front();
}

#endif // TEST_INTEROP_CXX_STDLIB_INPUTS_STD_VECTOR_H
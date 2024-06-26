#ifndef TEST_INTEROP_CXX_STDLIB_INPUTS_STD_SHARED_PTR_H
#define TEST_INTEROP_CXX_STDLIB_INPUTS_STD_SHARED_PTR_H

#include <memory>
#include <string>

using SharedPtrOfInt = std::shared_ptr<int>;
using SharedPtrOfString = std::shared_ptr<std::string>;

inline SharedPtrOfInt getSharedPtrOfInt() {
  return std::make_shared<int>(123);
}

inline SharedPtrOfString getSharedPtrOfString() {
  return std::make_shared<std::string>("abc123");
}

#endif // TEST_INTEROP_CXX_STDLIB_INPUTS_STD_SHARED_PTR_H

#ifndef TEST_INTEROP_CXX_STDLIB_INPUTS_STD_OPTIONAL_H
#define TEST_INTEROP_CXX_STDLIB_INPUTS_STD_OPTIONAL_H

#include <optional>

using StdOptionalInt = std::optional<int>;

inline StdOptionalInt getNonNilOptional() { return {123}; }

inline StdOptionalInt getNilOptional() { return {std::nullopt}; }

#endif // TEST_INTEROP_CXX_STDLIB_INPUTS_STD_OPTIONAL_H

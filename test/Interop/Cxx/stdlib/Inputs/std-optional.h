#ifndef TEST_INTEROP_CXX_STDLIB_INPUTS_STD_OPTIONAL_H
#define TEST_INTEROP_CXX_STDLIB_INPUTS_STD_OPTIONAL_H

#include <optional>
#include <string>

using StdOptionalInt = std::optional<int>;
using StdOptionalBool = std::optional<bool>;
using StdOptionalString = std::optional<std::string>;
using StdOptionalOptionalInt = std::optional<std::optional<int>>;

struct HasConstexprCtor {
  int value;
  constexpr HasConstexprCtor(int value) : value(value) {}
  constexpr HasConstexprCtor(const HasConstexprCtor &other) = default;
  constexpr HasConstexprCtor(HasConstexprCtor &&other) = default;
};
using StdOptionalHasConstexprCtor = std::optional<HasConstexprCtor>;

struct HasDeletedMoveCtor {
  int value;
  HasDeletedMoveCtor(int value) : value(value) {}
  HasDeletedMoveCtor(const HasDeletedMoveCtor &other) : value(other.value) {}
  HasDeletedMoveCtor(HasDeletedMoveCtor &&other) = delete;
};
using StdOptionalHasDeletedMoveCtor = std::optional<HasDeletedMoveCtor>;

inline StdOptionalInt getNonNilOptional() { return {123}; }

inline StdOptionalInt getNilOptional() { return {std::nullopt}; }

inline bool takesOptionalInt(std::optional<int> arg) { return (bool)arg; }
inline bool takesOptionalString(std::optional<std::string> arg) { return (bool)arg; }

#endif // TEST_INTEROP_CXX_STDLIB_INPUTS_STD_OPTIONAL_H

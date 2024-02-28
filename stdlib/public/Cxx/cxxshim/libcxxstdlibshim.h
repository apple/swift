#include <functional>
#include <string>
#ifdef _WIN32
#include <vector>
#endif

/// Used for std::string conformance to Swift.Hashable
typedef std::hash<std::string> __swift_interopHashOfString;

/// Used for std::u16string conformance to Swift.Hashable
typedef std::hash<std::u16string> __swift_interopHashOfU16String;

/// Used for std::u32string conformance to Swift.Hashable
typedef std::hash<std::u32string> __swift_interopHashOfU32String;

#ifdef _WIN32
template <class T>
ptrdiff_t operator-(const std::_Vector_iterator<T> &lhs,
                    const std::_Vector_iterator<T> &rhs);
#endif

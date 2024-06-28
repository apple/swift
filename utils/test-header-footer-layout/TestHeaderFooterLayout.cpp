#include "swift/Basic/HeaderFooterLayout.h"
#include <cstdint>
#include <type_traits>

using namespace swift;
using namespace std;

struct E {};

static_assert(sizeof(E) == 1 && alignof(E) == 1 && size_without_trailing_padding<E>::value == 0);

struct A1 { uint8_t x1; };
struct A2 { uint16_t x2; };
struct A3 { uint16_t x3_1; uint8_t x3_2; };
struct A4 { uint32_t x4; };
struct A5 { uint32_t x5_1; uint8_t x5_2; };
struct A6 { uint32_t x6_1; uint16_t x6_2; };
struct A7 { uint32_t x7_1; uint16_t x7_2; uint8_t x7_3; };
struct A8 { uint64_t x8; };

static_assert(is_standard_layout<A1>::value);
static_assert(is_standard_layout<A2>::value);
static_assert(is_standard_layout<A3>::value);
static_assert(is_standard_layout<A4>::value);
static_assert(is_standard_layout<A5>::value);
static_assert(is_standard_layout<A6>::value);
static_assert(is_standard_layout<A7>::value);
static_assert(is_standard_layout<A8>::value);

static_assert(sizeof(A1) == 1 && alignof(A1) == 1 && size_without_trailing_padding<A1>::value == 1);
static_assert(sizeof(A2) == 2 && alignof(A2) == 2 && size_without_trailing_padding<A2>::value == 2);
static_assert(sizeof(A3) == 4 && alignof(A3) == 2 && size_without_trailing_padding<A3>::value == 4);
static_assert(sizeof(A4) == 4 && alignof(A4) == 4 && size_without_trailing_padding<A4>::value == 4);
static_assert(sizeof(A5) == 8 && alignof(A5) == 4 && size_without_trailing_padding<A5>::value == 8);
static_assert(sizeof(A6) == 8 && alignof(A6) == 4 && size_without_trailing_padding<A6>::value == 8);
static_assert(sizeof(A7) == 8 && alignof(A7) == 4 && size_without_trailing_padding<A7>::value == 8);
static_assert(sizeof(A8) == 8 && alignof(A8) == 8 && size_without_trailing_padding<A8>::value == 8);

struct B3: A2, A1 {};
struct B5: A4, A1 {};
struct B6: A4, A2 {};
struct B7: A4, A2, A1 {};

static_assert(!is_standard_layout<B3>::value);
static_assert(!is_standard_layout<B5>::value);
static_assert(!is_standard_layout<B6>::value);
static_assert(!is_standard_layout<B7>::value);

static_assert(sizeof(B3) == 4 && alignof(B3) == 2 && size_without_trailing_padding<B3>::value == 3);
static_assert(sizeof(B5) == 8 && alignof(B5) == 4 && size_without_trailing_padding<B5>::value == 5);
static_assert(sizeof(B6) == 8 && alignof(B6) == 4 && size_without_trailing_padding<B6>::value == 6);
static_assert(sizeof(B7) == 8 && alignof(B7) == 4 && size_without_trailing_padding<B7>::value == 7);

namespace T1 {
struct X: HeaderFooterLayout<A1, A2, 12> {};
static_assert(sizeof(X::padding) == 9);
static_assert(sizeof(X) == 12 && alignof(X) == 2);
static_assert(size_without_trailing_padding<X>::value == 12);
static_assert(offsetof(X, x1) == 0);
static_assert(offsetof(X, x2) == 10);
}

namespace T2 {
struct X: HeaderFooterLayout<A2, E, 12> {};
static_assert(sizeof(X::padding) == 10);
static_assert(sizeof(X) == 12 && alignof(X) == 2);
static_assert(size_without_trailing_padding<X>::value == 12);
static_assert(offsetof(X, x2) == 0);
}

namespace T3 {
struct X: HeaderFooterLayout<E, A2, 12> {};
static_assert(sizeof(X::padding) == 10);
static_assert(sizeof(X) == 12 && alignof(X) == 2);
static_assert(size_without_trailing_padding<X>::value == 12);
static_assert(offsetof(X, x2) == 10);
}

namespace T4 {
struct X: HeaderFooterLayout<B3, E, 12> {};
static_assert(sizeof(X::padding) == 9);
static_assert(sizeof(X) == 12 && alignof(X) == 2);
static_assert(size_without_trailing_padding<X>::value == 12);
static_assert(offsetof(X, x2) == 0);
static_assert(offsetof(X, x1) == 2);
}

namespace T5 {
struct X: HeaderFooterLayout<E, B3, 12> {};
static_assert(sizeof(X::padding) == 8);
static_assert(sizeof(X) == 12 && alignof(X) == 2);
static_assert(size_without_trailing_padding<X>::value == 11);
static_assert(offsetof(X, x2) == 8);
static_assert(offsetof(X, x1) == 10);
}

namespace T6 {
struct X: HeaderFooterLayout<A8, B3, 12> {};
static_assert(sizeof(X) == 16 && alignof(X) == 8);
static_assert(size_without_trailing_padding<X>::value == 11);
static_assert(offsetof(X, x8) == 0);
static_assert(offsetof(X, x2) == 8);
static_assert(offsetof(X, x1) == 10);
}

int main() {
  return 0;
}

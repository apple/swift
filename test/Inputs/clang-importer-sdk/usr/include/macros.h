#include <macros_impl.h>
#include <macros_private_impl.h>
#include <header_guard.h>
#include <not_a_header_guard.h>

// Get Clang's NULL.
#include <stddef.h>

#define M_PI 3.14159265358979
#define M_PIf 3.14159265358979F
#define GL_FALSE 0
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define EOF (-1)
#define UINT32_MAX 0xFFFFFFFFU
#define INT64_MAX 0x7FFFFFFFFFFFFFFFLL
#if defined(_WIN32)
// MSVC compatibility will always return a signed value when the suffix is `LL`
// or `i64` and other targets will promote it to an unsigned type.
#define UINT64_MAX 0xFFFFFFFFFFFFFFFFULL
#else
#define UINT64_MAX 0xFFFFFFFFFFFFFFFFLL
#endif
#define MINUS_THREE -3
#define true 1
#define false 0
#define TRUE 1
#define FALSE 0

#define LL_TO_ULL 0x8000000000000000LL

#define A_PI M_PI

#define VERSION_STRING "Swift 1.0"
#define UTF8_STRING u8"Swift 🏃"
#define OBJC_STRING @"Unicode! ✨"
#define CF_STRING CFSTR("Swift")
#define INVALID_UTF8_STRING "\xFF\xFF\xFF\xFF\xFF\xFF"

#define INVALID_INTEGER_LITERAL_1 10_9
#define INVALID_INTEGER_LITERAL_2 10abc

// Check that macros that expand to macros from another module are imported
// correctly.
#define USES_MACRO_FROM_OTHER_MODULE_1 MACRO_FROM_IMPL
#define USES_MACRO_FROM_OTHER_MODULE_2 MACRO_FROM_PRIVATE_IMPL

// Should be suppressed during import.
#define NS_BLOCKS_AVAILABLE 1
#define CF_USE_OSBYTEORDER_H 1

#define NULL_VIA_NAME (NULL)
#define NULL_VIA_VALUE ((void *) 0)

#ifndef nil
# define nil ((id)0)
#endif
#define NULL_AS_NIL nil

#ifndef Nil
# define Nil ((Class)0)
#endif
#define NULL_AS_CLASS_NIL Nil

#define RECURSION RECURSION
#define REF_TO_RECURSION RECURSION

#define DISPATCH_TIME_NOW (0ull)
#define DISPATCH_TIME_FOREVER (~0ull)

// Bitwise Operations.

#define BIT_SHIFT_1 (1 << 0)
#define BIT_SHIFT_2 (1 << 2)
#define BIT_SHIFT_3 (3LL << 3)
#define BIT_SHIFT_4 (1U << 1)

#define STARTPOS_ATTRS 8
#define ATTR_BOLD      (1 << STARTPOS_ATTRS)
#define ATTR_ITALIC    (2 << STARTPOS_ATTRS)
#define ATTR_UNDERLINE (4 << STARTPOS_ATTRS)
#define ATTR_INVALID   (4 << MINUS_THREE) // Should skip. Negative shift.

#define RSHIFT_ONE     (UINT32_MAX >> 31)
#define RSHIFT_NEG     (MINUS_THREE >> 1)
#define RSHIFT_INVALID (0xFFFF >> MINUS_THREE) // Should skip. Negative shift.

#define XOR_HIGH (UINT64_MAX ^ UINT32_MAX)

// Integer Arithmetic.

#define ADD_ZERO        (0 + 0)
#define ADD_ONE         (ADD_ZERO + 1)
#define ADD_TWO         (ADD_ONE + ADD_ONE)
#define ADD_MINUS_TWO   (1 + MINUS_THREE)
#define ADD_MIXED_WIDTH (3 + 166LL)        // Result = 169LL.
#define ADD_MIXED_SIGN  (42U + 100LL)      // Result = 142LL.
#define ADD_UNDERFLOW   (0U + MINUS_THREE) // Result = (UINT32_MAX - 2)U.
#define ADD_OVERFLOW    (UINT32_MAX + 3)   // Result = 2U.

#define SUB_ONE         (ADD_TWO - 1)
#define SUB_ZERO        (SUB_ONE - SUB_ONE)
#define SUB_MINUS_ONE   (SUB_ZERO - SUB_ONE)
#define SUB_MIXED_WIDTH (64 - 22LL)                  // Result = 42LL.
#define SUB_MIXED_SIGN  (100U - 49)                  // Result = 51U.
#define SUB_UNDERFLOW   (0U - 1)                     // Result = (UINT32_MAX)U.
#define SUB_OVERFLOW    (UINT32_MAX - ADD_MINUS_TWO) // Result = 1U.

#define MULT_POS      (12 * 3)
#define MULT_NEG      (4 * MINUS_THREE)
#define MULT_MIXED_TYPES (2LL * UINT32_MAX) // Result = (2^33)LL

#define DIVIDE_INTEGRAL    (1024 / 8)
#define DIVIDE_NONINTEGRAL (3 / 2)
#define DIVIDE_MIXED_TYPES  (INT64_MAX / UINT32_MAX) // Result = (2^31)LL
#define DIVIDE_INVALID     (69 / 0) // Should skip. Divide by zero.

// Integer Comparisons.

#define EQUAL_FALSE            (99 == 66)
#define EQUAL_TRUE             (SUB_ONE == 1)
#define EQUAL_TRUE_MIXED_TYPES (UINT32_MAX == 4294967295LL)

#define GT_FALSE  (SUB_ONE > 50)
#define GT_TRUE   (INT64_MAX > UINT32_MAX)
#define MINUS_FIFTY (0 - 50)
#define GTE_FALSE (MINUS_FIFTY >= 50)
#define GTE_TRUE  (INT64_MAX >= UINT32_MAX)

#define LT_FALSE  (SUB_UNDERFLOW < UINT32_MAX)
#define MINUS_NINETY_NINE (-99)
#define MINUS_FORTY_TWO   (-42)
#define LT_TRUE   (MINUS_NINETY_NINE < MINUS_FORTY_TWO)
#define LTE_FALSE (ADD_ONE <= 0)
#define LTE_TRUE  (SUB_UNDERFLOW <= UINT32_MAX)

// Logical Comparisons

#define L_AND_TRUE    (1 && 1)
#define L_AND_FALSE   (0 && 1)
#define L_AND_TRUE_B  (EQUAL_TRUE && EQUAL_TRUE_MIXED_TYPES)
#define L_AND_FALSE_B (EQUAL_FALSE && EQUAL_TRUE)

#define L_OR_TRUE     (0 || 1)
#define L_OR_FALSE    (0 || 0)
#define L_OR_TRUE_B   (EQUAL_FALSE || EQUAL_TRUE)
#define L_OR_FALSE_B  (EQUAL_FALSE || L_OR_FALSE)

// Recursion with expressions
#define RECURSION_WITH_EXPR RECURSION_WITH_EXPR_HELPER + 1
#define RECURSION_WITH_EXPR_HELPER RECURSION_WITH_EXPR

#define RECURSION_WITH_EXPR2 RECURSION_WITH_EXPR2_HELPER
#define RECURSION_WITH_EXPR2_HELPER RECURSION_WITH_EXPR2 + 1

#define RECURSION_WITH_EXPR3 RECURSION_WITH_EXPR3_HELPER + 1
#define RECURSION_WITH_EXPR3_HELPER RECURSION_WITH_EXPR3 + 1

// casts
#define HIGH_C (char)1
#define LOW_C (char)0
#define FALLING_EDGE (unsigned char)2
#define RISING_EDGE (unsigned char)3
#define D0 (unsigned char)0
#define D1 (unsigned char)1

// Casts with problematic types
#define UNAVAILABLE_ONE ((unavailable_t)1)
typedef unsigned unavailable_t __attribute__((unavailable));
#define DEPRECATED_ONE ((deprecated_t)1)
typedef unsigned deprecated_t __attribute__((deprecated));
#define OKAY_TYPED_ONE ((okay_t)1)
typedef unsigned okay_t;

#define FUNC_LIKE_MACRO() 0
#define FUNC_LIKE_MACRO_2(PARAM) PARAM

// Unsupported binary arithmetic
#define INVALID_ARITHMETIC_1 5 + INVALID_INTEGER_LITERAL_1
#define INVALID_ARITHMETIC_2 INVALID_INTEGER_LITERAL_1 + ADD_TWO
#define INVALID_ARITHMETIC_3 ADD_TWO + INVALID_INTEGER_LITERAL_1
#define INVALID_ARITHMETIC_4                                                   \
  INVALID_INTEGER_LITERAL_1 - INVALID_INTEGER_LITERAL_1
#define INVALID_ARITHMETIC_5 1 + VERSION_STRING
#define INVALID_ARITHMETIC_6 1 + 'c'
#define INVALID_ARITHMETIC_7 3 % 2

// Unsupported literals
#define CHAR_LITERAL 'a'

// Unsupported macro structures
#define UNSUPPORTED_1 FUNC_LIKE_MACRO()
#define UNSUPPORTED_2 FUNC_LIKE_MACRO_2(1)
#define UNSUPPORTED_3 1 + FUNC_LIKE_MACRO_2(1)
#define UNSUPPORTED_4                                                          \
  extern bool globalFlag;                                                      \
  if (globalFlag) {                                                            \
    /* do something */                                                         \
  }
#define UNSUPPORTED_5 1 + 1 + 1

//===--- UnicodeShims.h - Access to Unicode data for the core stdlib ------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  Data structures required for Unicode support in Swift that are
//  statically initialized in its runtime's C++ source.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_STDLIB_SHIMS_UNICODESHIMS_H_
#define SWIFT_STDLIB_SHIMS_UNICODESHIMS_H_

#include "SwiftStdint.h"
#include "SwiftStdbool.h"
#include "Visibility.h"

#if __has_feature(nullability)
#pragma clang assume_nonnull begin
#endif

#ifdef __cplusplus
extern "C" {
#endif

SWIFT_RUNTIME_STDLIB_API
__swift_int32_t _swift_stdlib_unicode_strToUpper(
  __swift_uint16_t *Destination, __swift_int32_t DestinationCapacity,
  const __swift_uint16_t *Source, __swift_int32_t SourceLength);

SWIFT_RUNTIME_STDLIB_API
__swift_int32_t _swift_stdlib_unicode_strToLower(
  __swift_uint16_t *Destination, __swift_int32_t DestinationCapacity,
  const __swift_uint16_t *Source, __swift_int32_t SourceLength);

typedef enum __swift_stdlib_UProperty {
  __swift_stdlib_UCHAR_ALPHABETIC = 0,
  __swift_stdlib_UCHAR_BINARY_START = __swift_stdlib_UCHAR_ALPHABETIC,
  __swift_stdlib_UCHAR_ASCII_HEX_DIGIT = 1,
  __swift_stdlib_UCHAR_BIDI_CONTROL = 2,
  __swift_stdlib_UCHAR_BIDI_MIRRORED = 3,
  __swift_stdlib_UCHAR_DASH = 4,
  __swift_stdlib_UCHAR_DEFAULT_IGNORABLE_CODE_POINT = 5,
  __swift_stdlib_UCHAR_DEPRECATED = 6,
  __swift_stdlib_UCHAR_DIACRITIC = 7,
  __swift_stdlib_UCHAR_EXTENDER = 8,
  __swift_stdlib_UCHAR_FULL_COMPOSITION_EXCLUSION = 9,
  __swift_stdlib_UCHAR_GRAPHEME_BASE = 10,
  __swift_stdlib_UCHAR_GRAPHEME_EXTEND = 11,
  __swift_stdlib_UCHAR_GRAPHEME_LINK = 12,
  __swift_stdlib_UCHAR_HEX_DIGIT = 13,
  __swift_stdlib_UCHAR_HYPHEN = 14,
  __swift_stdlib_UCHAR_ID_CONTINUE = 15,
  __swift_stdlib_UCHAR_ID_START = 16,
  __swift_stdlib_UCHAR_IDEOGRAPHIC = 17,
  __swift_stdlib_UCHAR_IDS_BINARY_OPERATOR = 18,
  __swift_stdlib_UCHAR_IDS_TRINARY_OPERATOR = 19,
  __swift_stdlib_UCHAR_JOIN_CONTROL = 20,
  __swift_stdlib_UCHAR_LOGICAL_ORDER_EXCEPTION = 21,
  __swift_stdlib_UCHAR_LOWERCASE = 22,
  __swift_stdlib_UCHAR_MATH = 23,
  __swift_stdlib_UCHAR_NONCHARACTER_CODE_POINT = 24,
  __swift_stdlib_UCHAR_QUOTATION_MARK = 25,
  __swift_stdlib_UCHAR_RADICAL = 26,
  __swift_stdlib_UCHAR_SOFT_DOTTED = 27,
  __swift_stdlib_UCHAR_TERMINAL_PUNCTUATION = 28,
  __swift_stdlib_UCHAR_UNIFIED_IDEOGRAPH = 29,
  __swift_stdlib_UCHAR_UPPERCASE = 30,
  __swift_stdlib_UCHAR_WHITE_SPACE = 31,
  __swift_stdlib_UCHAR_XID_CONTINUE = 32,
  __swift_stdlib_UCHAR_XID_START = 33,
  __swift_stdlib_UCHAR_CASE_SENSITIVE = 34,
  __swift_stdlib_UCHAR_S_TERM = 35,
  __swift_stdlib_UCHAR_VARIATION_SELECTOR = 36,
  __swift_stdlib_UCHAR_NFD_INERT = 37,
  __swift_stdlib_UCHAR_NFKD_INERT = 38,
  __swift_stdlib_UCHAR_NFC_INERT = 39,
  __swift_stdlib_UCHAR_NFKC_INERT = 40,
  __swift_stdlib_UCHAR_SEGMENT_STARTER = 41,
  __swift_stdlib_UCHAR_PATTERN_SYNTAX = 42,
  __swift_stdlib_UCHAR_PATTERN_WHITE_SPACE = 43,
  __swift_stdlib_UCHAR_POSIX_ALNUM = 44,
  __swift_stdlib_UCHAR_POSIX_BLANK = 45,
  __swift_stdlib_UCHAR_POSIX_GRAPH = 46,
  __swift_stdlib_UCHAR_POSIX_PRINT = 47,
  __swift_stdlib_UCHAR_POSIX_XDIGIT = 48,
  __swift_stdlib_UCHAR_CASED = 49,
  __swift_stdlib_UCHAR_CASE_IGNORABLE = 50,
  __swift_stdlib_UCHAR_CHANGES_WHEN_LOWERCASED = 51,
  __swift_stdlib_UCHAR_CHANGES_WHEN_UPPERCASED = 52,
  __swift_stdlib_UCHAR_CHANGES_WHEN_TITLECASED = 53,
  __swift_stdlib_UCHAR_CHANGES_WHEN_CASEFOLDED = 54,
  __swift_stdlib_UCHAR_CHANGES_WHEN_CASEMAPPED = 55,
  __swift_stdlib_UCHAR_CHANGES_WHEN_NFKC_CASEFOLDED = 56,
  __swift_stdlib_UCHAR_EMOJI = 57,
  __swift_stdlib_UCHAR_EMOJI_PRESENTATION = 58,
  __swift_stdlib_UCHAR_EMOJI_MODIFIER = 59,
  __swift_stdlib_UCHAR_EMOJI_MODIFIER_BASE = 60,

  __swift_stdlib_UCHAR_BIDI_CLASS = 0x1000,
  __swift_stdlib_UCHAR_INT_START = __swift_stdlib_UCHAR_BIDI_CLASS,
  __swift_stdlib_UCHAR_BLOCK = 0x1001,
  __swift_stdlib_UCHAR_CANONICAL_COMBINING_CLASS = 0x1002,
  __swift_stdlib_UCHAR_DECOMPOSITION_TYPE = 0x1003,
  __swift_stdlib_UCHAR_EAST_ASIAN_WIDTH = 0x1004,
  __swift_stdlib_UCHAR_GENERAL_CATEGORY = 0x1005,
  __swift_stdlib_UCHAR_JOINING_GROUP = 0x1006,
  __swift_stdlib_UCHAR_JOINING_TYPE = 0x1007,
  __swift_stdlib_UCHAR_LINE_BREAK = 0x1008,
  __swift_stdlib_UCHAR_NUMERIC_TYPE = 0x1009,
  __swift_stdlib_UCHAR_SCRIPT = 0x100A,
  __swift_stdlib_UCHAR_HANGUL_SYLLABLE_TYPE = 0x100B,
  __swift_stdlib_UCHAR_NFD_QUICK_CHECK = 0x100C,
  __swift_stdlib_UCHAR_NFKD_QUICK_CHECK = 0x100D,
  __swift_stdlib_UCHAR_NFC_QUICK_CHECK = 0x100E,
  __swift_stdlib_UCHAR_NFKC_QUICK_CHECK = 0x100F,
  __swift_stdlib_UCHAR_LEAD_CANONICAL_COMBINING_CLASS = 0x1010,
  __swift_stdlib_UCHAR_TRAIL_CANONICAL_COMBINING_CLASS = 0x1011,
  __swift_stdlib_UCHAR_GRAPHEME_CLUSTER_BREAK = 0x1012,
  __swift_stdlib_UCHAR_SENTENCE_BREAK = 0x1013,
  __swift_stdlib_UCHAR_WORD_BREAK = 0x1014,
  __swift_stdlib_UCHAR_BIDI_PAIRED_BRACKET_TYPE = 0x1015,

  __swift_stdlib_UCHAR_GENERAL_CATEGORY_MASK = 0x2000,
  __swift_stdlib_UCHAR_MASK_START = __swift_stdlib_UCHAR_GENERAL_CATEGORY_MASK,

  __swift_stdlib_UCHAR_NUMERIC_VALUE = 0x3000,
  __swift_stdlib_UCHAR_DOUBLE_START = __swift_stdlib_UCHAR_NUMERIC_VALUE,

  __swift_stdlib_UCHAR_AGE = 0x4000,
  __swift_stdlib_UCHAR_STRING_START = __swift_stdlib_UCHAR_AGE,
  __swift_stdlib_UCHAR_BIDI_MIRRORING_GLYPH = 0x4001,
  __swift_stdlib_UCHAR_CASE_FOLDING = 0x4002,

  __swift_stdlib_UCHAR_LOWERCASE_MAPPING = 0x4004,
  __swift_stdlib_UCHAR_NAME = 0x4005,
  __swift_stdlib_UCHAR_SIMPLE_CASE_FOLDING = 0x4006,
  __swift_stdlib_UCHAR_SIMPLE_LOWERCASE_MAPPING = 0x4007,
  __swift_stdlib_UCHAR_SIMPLE_TITLECASE_MAPPING = 0x4008,
  __swift_stdlib_UCHAR_SIMPLE_UPPERCASE_MAPPING = 0x4009,
  __swift_stdlib_UCHAR_TITLECASE_MAPPING = 0x400A,

  __swift_stdlib_UCHAR_UPPERCASE_MAPPING = 0x400C,
  __swift_stdlib_UCHAR_BIDI_PAIRED_BRACKET = 0x400D,

  __swift_stdlib_UCHAR_SCRIPT_EXTENSIONS = 0x7000,
  __swift_stdlib_UCHAR_OTHER_PROPERTY_START =
      __swift_stdlib_UCHAR_SCRIPT_EXTENSIONS,

  __swift_stdlib_UCHAR_INVALID_CODE = -1
} __swift_stdlib_UProperty;

typedef enum __swift_stdlib_UErrorCode {
  __swift_stdlib_U_USING_FALLBACK_WARNING = -128,
  __swift_stdlib_U_ERROR_WARNING_START = -128,
  __swift_stdlib_U_USING_DEFAULT_WARNING = -127,
  __swift_stdlib_U_SAFECLONE_ALLOCATED_WARNING = -126,
  __swift_stdlib_U_STATE_OLD_WARNING = -125,
  __swift_stdlib_U_STRING_NOT_TERMINATED_WARNING = -124,
  __swift_stdlib_U_SORT_KEY_TOO_SHORT_WARNING = -123,
  __swift_stdlib_U_AMBIGUOUS_ALIAS_WARNING = -122,
  __swift_stdlib_U_DIFFERENT_UCA_VERSION = -121,
  __swift_stdlib_U_PLUGIN_CHANGED_LEVEL_WARNING = -120,
  __swift_stdlib_U_ERROR_WARNING_LIMIT,
  __swift_stdlib_U_ZERO_ERROR = 0,
  __swift_stdlib_U_ILLEGAL_ARGUMENT_ERROR = 1,
  __swift_stdlib_U_MISSING_RESOURCE_ERROR = 2,
  __swift_stdlib_U_INVALID_FORMAT_ERROR = 3,
  __swift_stdlib_U_FILE_ACCESS_ERROR = 4,
  __swift_stdlib_U_INTERNAL_PROGRAM_ERROR = 5,
  __swift_stdlib_U_MESSAGE_PARSE_ERROR = 6,
  __swift_stdlib_U_MEMORY_ALLOCATION_ERROR = 7,
  __swift_stdlib_U_INDEX_OUTOFBOUNDS_ERROR = 8,
  __swift_stdlib_U_PARSE_ERROR = 9,
  __swift_stdlib_U_INVALID_CHAR_FOUND = 10,
  __swift_stdlib_U_TRUNCATED_CHAR_FOUND = 11,
  __swift_stdlib_U_ILLEGAL_CHAR_FOUND = 12,
  __swift_stdlib_U_INVALID_TABLE_FORMAT = 13,
  __swift_stdlib_U_INVALID_TABLE_FILE = 14,
  __swift_stdlib_U_BUFFER_OVERFLOW_ERROR = 15,
  __swift_stdlib_U_UNSUPPORTED_ERROR = 16,
  __swift_stdlib_U_RESOURCE_TYPE_MISMATCH = 17,
  __swift_stdlib_U_ILLEGAL_ESCAPE_SEQUENCE = 18,
  __swift_stdlib_U_UNSUPPORTED_ESCAPE_SEQUENCE = 19,
  __swift_stdlib_U_NO_SPACE_AVAILABLE = 20,
  __swift_stdlib_U_CE_NOT_FOUND_ERROR = 21,
  __swift_stdlib_U_PRIMARY_TOO_LONG_ERROR = 22,
  __swift_stdlib_U_STATE_TOO_OLD_ERROR = 23,
  __swift_stdlib_U_TOO_MANY_ALIASES_ERROR = 24,
  __swift_stdlib_U_ENUM_OUT_OF_SYNC_ERROR = 25,
  __swift_stdlib_U_INVARIANT_CONVERSION_ERROR = 26,
  __swift_stdlib_U_INVALID_STATE_ERROR = 27,
  __swift_stdlib_U_COLLATOR_VERSION_MISMATCH = 28,
  __swift_stdlib_U_USELESS_COLLATOR_ERROR = 29,
  __swift_stdlib_U_NO_WRITE_PERMISSION = 30,
  __swift_stdlib_U_STANDARD_ERROR_LIMIT,
  __swift_stdlib_U_BAD_VARIABLE_DEFINITION = 0x10000,
  __swift_stdlib_U_PARSE_ERROR_START = 0x10000,
  __swift_stdlib_U_MALFORMED_RULE,
  __swift_stdlib_U_MALFORMED_SET,
  __swift_stdlib_U_MALFORMED_SYMBOL_REFERENCE,
  __swift_stdlib_U_MALFORMED_UNICODE_ESCAPE,
  __swift_stdlib_U_MALFORMED_VARIABLE_DEFINITION,
  __swift_stdlib_U_MALFORMED_VARIABLE_REFERENCE,
  __swift_stdlib_U_MISMATCHED_SEGMENT_DELIMITERS,
  __swift_stdlib_U_MISPLACED_ANCHOR_START,
  __swift_stdlib_U_MISPLACED_CURSOR_OFFSET,
  __swift_stdlib_U_MISPLACED_QUANTIFIER,
  __swift_stdlib_U_MISSING_OPERATOR,
  __swift_stdlib_U_MISSING_SEGMENT_CLOSE,
  __swift_stdlib_U_MULTIPLE_ANTE_CONTEXTS,
  __swift_stdlib_U_MULTIPLE_CURSORS,
  __swift_stdlib_U_MULTIPLE_POST_CONTEXTS,
  __swift_stdlib_U_TRAILING_BACKSLASH,
  __swift_stdlib_U_UNDEFINED_SEGMENT_REFERENCE,
  __swift_stdlib_U_UNDEFINED_VARIABLE,
  __swift_stdlib_U_UNQUOTED_SPECIAL,
  __swift_stdlib_U_UNTERMINATED_QUOTE,
  __swift_stdlib_U_RULE_MASK_ERROR,
  __swift_stdlib_U_MISPLACED_COMPOUND_FILTER,
  __swift_stdlib_U_MULTIPLE_COMPOUND_FILTERS,
  __swift_stdlib_U_INVALID_RBT_SYNTAX,
  __swift_stdlib_U_INVALID_PROPERTY_PATTERN,
  __swift_stdlib_U_MALFORMED_PRAGMA,
  __swift_stdlib_U_UNCLOSED_SEGMENT,
  __swift_stdlib_U_ILLEGAL_CHAR_IN_SEGMENT,
  __swift_stdlib_U_VARIABLE_RANGE_EXHAUSTED,
  __swift_stdlib_U_VARIABLE_RANGE_OVERLAP,
  __swift_stdlib_U_ILLEGAL_CHARACTER,
  __swift_stdlib_U_INTERNAL_TRANSLITERATOR_ERROR,
  __swift_stdlib_U_INVALID_ID,
  __swift_stdlib_U_INVALID_FUNCTION,
  __swift_stdlib_U_PARSE_ERROR_LIMIT,
  __swift_stdlib_U_UNEXPECTED_TOKEN = 0x10100,
  __swift_stdlib_U_FMT_PARSE_ERROR_START = 0x10100,
  __swift_stdlib_U_MULTIPLE_DECIMAL_SEPARATORS,
  __swift_stdlib_U_MULTIPLE_DECIMAL_SEPERATORS =
      __swift_stdlib_U_MULTIPLE_DECIMAL_SEPARATORS,
  __swift_stdlib_U_MULTIPLE_EXPONENTIAL_SYMBOLS,
  __swift_stdlib_U_MALFORMED_EXPONENTIAL_PATTERN,
  __swift_stdlib_U_MULTIPLE_PERCENT_SYMBOLS,
  __swift_stdlib_U_MULTIPLE_PERMILL_SYMBOLS,
  __swift_stdlib_U_MULTIPLE_PAD_SPECIFIERS,
  __swift_stdlib_U_PATTERN_SYNTAX_ERROR,
  __swift_stdlib_U_ILLEGAL_PAD_POSITION,
  __swift_stdlib_U_UNMATCHED_BRACES,
  __swift_stdlib_U_UNSUPPORTED_PROPERTY,
  __swift_stdlib_U_UNSUPPORTED_ATTRIBUTE,
  __swift_stdlib_U_ARGUMENT_TYPE_MISMATCH,
  __swift_stdlib_U_DUPLICATE_KEYWORD,
  __swift_stdlib_U_UNDEFINED_KEYWORD,
  __swift_stdlib_U_DEFAULT_KEYWORD_MISSING,
  __swift_stdlib_U_DECIMAL_NUMBER_SYNTAX_ERROR,
  __swift_stdlib_U_FORMAT_INEXACT_ERROR,
  __swift_stdlib_U_FMT_PARSE_ERROR_LIMIT,
  __swift_stdlib_U_BRK_INTERNAL_ERROR = 0x10200,
  __swift_stdlib_U_BRK_ERROR_START = 0x10200,
  __swift_stdlib_U_BRK_HEX_DIGITS_EXPECTED,
  __swift_stdlib_U_BRK_SEMICOLON_EXPECTED,
  __swift_stdlib_U_BRK_RULE_SYNTAX,
  __swift_stdlib_U_BRK_UNCLOSED_SET,
  __swift_stdlib_U_BRK_ASSIGN_ERROR,
  __swift_stdlib_U_BRK_VARIABLE_REDFINITION,
  __swift_stdlib_U_BRK_MISMATCHED_PAREN,
  __swift_stdlib_U_BRK_NEW_LINE_IN_QUOTED_STRING,
  __swift_stdlib_U_BRK_UNDEFINED_VARIABLE,
  __swift_stdlib_U_BRK_INIT_ERROR,
  __swift_stdlib_U_BRK_RULE_EMPTY_SET,
  __swift_stdlib_U_BRK_UNRECOGNIZED_OPTION,
  __swift_stdlib_U_BRK_MALFORMED_RULE_TAG,
  __swift_stdlib_U_BRK_ERROR_LIMIT,
  __swift_stdlib_U_REGEX_INTERNAL_ERROR = 0x10300,
  __swift_stdlib_U_REGEX_ERROR_START = 0x10300,
  __swift_stdlib_U_REGEX_RULE_SYNTAX,
  __swift_stdlib_U_REGEX_INVALID_STATE,
  __swift_stdlib_U_REGEX_BAD_ESCAPE_SEQUENCE,
  __swift_stdlib_U_REGEX_PROPERTY_SYNTAX,
  __swift_stdlib_U_REGEX_UNIMPLEMENTED,
  __swift_stdlib_U_REGEX_MISMATCHED_PAREN,
  __swift_stdlib_U_REGEX_NUMBER_TOO_BIG,
  __swift_stdlib_U_REGEX_BAD_INTERVAL,
  __swift_stdlib_U_REGEX_MAX_LT_MIN,
  __swift_stdlib_U_REGEX_INVALID_BACK_REF,
  __swift_stdlib_U_REGEX_INVALID_FLAG,
  __swift_stdlib_U_REGEX_LOOK_BEHIND_LIMIT,
  __swift_stdlib_U_REGEX_SET_CONTAINS_STRING,
#ifndef __swift_stdlib_U_HIDE_DEPRECATED_API
  __swift_stdlib_U_REGEX_OCTAL_TOO_BIG,
#endif
  __swift_stdlib_U_REGEX_MISSING_CLOSE_BRACKET =
      __swift_stdlib_U_REGEX_SET_CONTAINS_STRING + 2,
  __swift_stdlib_U_REGEX_INVALID_RANGE,
  __swift_stdlib_U_REGEX_STACK_OVERFLOW,
  __swift_stdlib_U_REGEX_TIME_OUT,
  __swift_stdlib_U_REGEX_STOPPED_BY_CALLER,
#ifndef __swift_stdlib_U_HIDE_DRAFT_API
  __swift_stdlib_U_REGEX_PATTERN_TOO_BIG,
  __swift_stdlib_U_REGEX_INVALID_CAPTURE_GROUP_NAME,
#endif
  __swift_stdlib_U_REGEX_ERROR_LIMIT =
      __swift_stdlib_U_REGEX_STOPPED_BY_CALLER + 3,
  __swift_stdlib_U_IDNA_PROHIBITED_ERROR = 0x10400,
  __swift_stdlib_U_IDNA_ERROR_START = 0x10400,
  __swift_stdlib_U_IDNA_UNASSIGNED_ERROR,
  __swift_stdlib_U_IDNA_CHECK_BIDI_ERROR,
  __swift_stdlib_U_IDNA_STD3_ASCII_RULES_ERROR,
  __swift_stdlib_U_IDNA_ACE_PREFIX_ERROR,
  __swift_stdlib_U_IDNA_VERIFICATION_ERROR,
  __swift_stdlib_U_IDNA_LABEL_TOO_LONG_ERROR,
  __swift_stdlib_U_IDNA_ZERO_LENGTH_LABEL_ERROR,
  __swift_stdlib_U_IDNA_DOMAIN_NAME_TOO_LONG_ERROR,
  __swift_stdlib_U_IDNA_ERROR_LIMIT,
  __swift_stdlib_U_STRINGPREP_PROHIBITED_ERROR =
      __swift_stdlib_U_IDNA_PROHIBITED_ERROR,
  __swift_stdlib_U_STRINGPREP_UNASSIGNED_ERROR =
      __swift_stdlib_U_IDNA_UNASSIGNED_ERROR,
  __swift_stdlib_U_STRINGPREP_CHECK_BIDI_ERROR =
      __swift_stdlib_U_IDNA_CHECK_BIDI_ERROR,
  __swift_stdlib_U_PLUGIN_ERROR_START = 0x10500,
  __swift_stdlib_U_PLUGIN_TOO_HIGH = 0x10500,
  __swift_stdlib_U_PLUGIN_DIDNT_SET_LEVEL,
  __swift_stdlib_U_PLUGIN_ERROR_LIMIT,
  __swift_stdlib_U_ERROR_LIMIT = __swift_stdlib_U_PLUGIN_ERROR_LIMIT
} __swift_stdlib_UErrorCode;

typedef enum __swift_stdlib_UCharCategory {
  __swift_stdlib_U_UNASSIGNED              = 0,
  __swift_stdlib_U_GENERAL_OTHER_TYPES     = 0,
  __swift_stdlib_U_UPPERCASE_LETTER        = 1,
  __swift_stdlib_U_LOWERCASE_LETTER        = 2,
  __swift_stdlib_U_TITLECASE_LETTER        = 3,
  __swift_stdlib_U_MODIFIER_LETTER         = 4,
  __swift_stdlib_U_OTHER_LETTER            = 5,
  __swift_stdlib_U_NON_SPACING_MARK        = 6,
  __swift_stdlib_U_ENCLOSING_MARK          = 7,
  __swift_stdlib_U_COMBINING_SPACING_MARK  = 8,
  __swift_stdlib_U_DECIMAL_DIGIT_NUMBER    = 9,
  __swift_stdlib_U_LETTER_NUMBER           = 10,
  __swift_stdlib_U_OTHER_NUMBER            = 11,
  __swift_stdlib_U_SPACE_SEPARATOR         = 12,
  __swift_stdlib_U_LINE_SEPARATOR          = 13,
  __swift_stdlib_U_PARAGRAPH_SEPARATOR     = 14,
  __swift_stdlib_U_CONTROL_CHAR            = 15,
  __swift_stdlib_U_FORMAT_CHAR             = 16,
  __swift_stdlib_U_PRIVATE_USE_CHAR        = 17,
  __swift_stdlib_U_SURROGATE               = 18,
  __swift_stdlib_U_DASH_PUNCTUATION        = 19,
  __swift_stdlib_U_START_PUNCTUATION       = 20,
  __swift_stdlib_U_END_PUNCTUATION         = 21,
  __swift_stdlib_U_CONNECTOR_PUNCTUATION   = 22,
  __swift_stdlib_U_OTHER_PUNCTUATION       = 23,
  __swift_stdlib_U_MATH_SYMBOL             = 24,
  __swift_stdlib_U_CURRENCY_SYMBOL         = 25,
  __swift_stdlib_U_MODIFIER_SYMBOL         = 26,
  __swift_stdlib_U_OTHER_SYMBOL            = 27,
  __swift_stdlib_U_INITIAL_PUNCTUATION     = 28,
  __swift_stdlib_U_FINAL_PUNCTUATION       = 29,
  __swift_stdlib_U_CHAR_CATEGORY_COUNT
} __swift_stdlib_UCharCategory;

typedef enum __swift_stdlib_UCharNameChoice {
  __swift_stdlib_U_UNICODE_CHAR_NAME,
#ifndef U_HIDE_DEPRECATED_API
  __swift_stdlib_U_UNICODE_10_CHAR_NAME,
#endif
  __swift_stdlib_U_EXTENDED_CHAR_NAME = __swift_stdlib_U_UNICODE_CHAR_NAME + 2,
  __swift_stdlib_U_CHAR_NAME_ALIAS,
#ifndef U_HIDE_DEPRECATED_API
  __swift_stdlib_U_CHAR_NAME_CHOICE_COUNT
#endif
} __swift_stdlib_UCharNameChoice;

typedef enum __swift_stdlib_UNumericType {
  __swift_stdlib_U_NT_NONE,
  __swift_stdlib_U_NT_DECIMAL,
  __swift_stdlib_U_NT_DIGIT,
  __swift_stdlib_U_NT_NUMERIC,
#ifndef U_HIDE_DEPRECATED_API
  __swift_stdlib_U_NT_COUNT
#endif
} __swift_stdlib_UNumericType;

typedef struct __swift_stdlib_UBreakIterator __swift_stdlib_UBreakIterator;
typedef struct __swift_stdlib_UText __swift_stdlib_UText;
typedef struct __swift_stdlib_UNormalizer2 __swift_stdlib_UNormalizer2;
typedef __swift_int8_t __swift_stdlib_UBool;
typedef __swift_int32_t __swift_stdlib_UChar32;
#if defined(__APPLE__)
typedef __swift_uint16_t __swift_stdlib_UChar;
#else
#if defined(__cplusplus)
typedef char16_t __swift_stdlib_UChar;
#else
typedef __swift_uint16_t __swift_stdlib_UChar;
#endif
#endif
#define __SWIFT_STDLIB_U_MAX_VERSION_LENGTH 4
typedef __swift_uint8_t
    __swift_stdlib_UVersionInfo[__SWIFT_STDLIB_U_MAX_VERSION_LENGTH];

SWIFT_RUNTIME_STDLIB_API
__swift_stdlib_UBool
__swift_stdlib_unorm2_hasBoundaryBefore(const __swift_stdlib_UNormalizer2 *,
                                        __swift_stdlib_UChar32);

SWIFT_RUNTIME_STDLIB_API
const __swift_stdlib_UNormalizer2 *
__swift_stdlib_unorm2_getNFCInstance(__swift_stdlib_UErrorCode *);

SWIFT_RUNTIME_STDLIB_API
__swift_int32_t
__swift_stdlib_unorm2_normalize(const __swift_stdlib_UNormalizer2 *,
                                const __swift_stdlib_UChar *, __swift_int32_t,
                                __swift_stdlib_UChar *, __swift_int32_t,
                                __swift_stdlib_UErrorCode *);

SWIFT_RUNTIME_STDLIB_API
__swift_int32_t __swift_stdlib_unorm2_spanQuickCheckYes(
    const __swift_stdlib_UNormalizer2 *, const __swift_stdlib_UChar *,
    __swift_int32_t, __swift_stdlib_UErrorCode *);

SWIFT_RUNTIME_STDLIB_API
__swift_stdlib_UBool
    __swift_stdlib_u_hasBinaryProperty(__swift_stdlib_UChar32,
                                       __swift_stdlib_UProperty);

SWIFT_RUNTIME_STDLIB_API
void __swift_stdlib_u_charAge(
    __swift_stdlib_UChar32, __swift_stdlib_UVersionInfo _Nonnull);

SWIFT_RUNTIME_STDLIB_API
__swift_int32_t
    __swift_stdlib_u_getIntPropertyValue(__swift_stdlib_UChar32,
                                         __swift_stdlib_UProperty);

SWIFT_RUNTIME_STDLIB_API
__swift_int32_t __swift_stdlib_u_charName(
    __swift_stdlib_UChar32 code, __swift_stdlib_UCharNameChoice nameChoice,
    char *_Nullable buffer, __swift_int32_t bufferLength,
    __swift_stdlib_UErrorCode *pErrorCode);

SWIFT_RUNTIME_STDLIB_API
__swift_int32_t __swift_stdlib_u_strToLower(
    __swift_stdlib_UChar *dest, __swift_int32_t destCapacity,
    const __swift_stdlib_UChar *src, __swift_int32_t srcLength,
    const char *_Nullable locale, __swift_stdlib_UErrorCode *pErrorCode);

SWIFT_RUNTIME_STDLIB_API
__swift_int32_t __swift_stdlib_u_strToTitle(
    __swift_stdlib_UChar *dest, __swift_int32_t destCapacity,
    const __swift_stdlib_UChar *src, __swift_int32_t srcLength,
    __swift_stdlib_UBreakIterator *_Nullable titleIter,
    const char *_Nullable locale, __swift_stdlib_UErrorCode *pErrorCode);

SWIFT_RUNTIME_STDLIB_API
__swift_int32_t __swift_stdlib_u_strToUpper(
    __swift_stdlib_UChar *dest, __swift_int32_t destCapacity,
    const __swift_stdlib_UChar *src, __swift_int32_t srcLength,
    const char *_Nullable locale, __swift_stdlib_UErrorCode *pErrorCode);

SWIFT_RUNTIME_STDLIB_API
double __swift_stdlib_u_getNumericValue(__swift_stdlib_UChar32 c);


#ifdef __cplusplus
} // extern "C"
#endif

#if __has_feature(nullability)
#pragma clang assume_nonnull end
#endif

#endif

//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#if SWIFT_STDLIB_ENABLE_UNICODE_DATA
#include "Common/WordData.h"
#else
#include "swift/Runtime/Debug.h"
#endif
#include "swift/shims/UnicodeData.h"
#include <stdint.h>

SWIFT_RUNTIME_STDLIB_INTERNAL
__swift_uint8_t _swift_stdlib_getWordBreakProperty(__swift_uint32_t scalar) {
#if !SWIFT_STDLIB_ENABLE_UNICODE_DATA
  swift::swift_abortDisabledUnicodeSupport();
#else
  auto index = 1; //0th element is a dummy element
  while (index < WORD_BREAK_DATA_COUNT) {
    auto entry = _swift_stdlib_words[index];

    // Shift the range count out of the value.
    auto lower = (entry << 11) >> 11;
    
    // Shift the enum out first, then shift out the scalar value.
    auto upper = lower + (entry >> 21) - 1;

    //If we want the left child of the current node in our virtual tree,
    //that's at index * 2, if we want the right child it's at (index * 2) + 1
    index = scalar < lower ?
      2 * index :
      scalar <= upper ?
        (WORD_BREAK_DATA_COUNT * 2) + _swift_stdlib_words_data[index] :
        2 * index + 1;
  }
  if (index >= WORD_BREAK_DATA_COUNT * 2) {
    return (__swift_uint8_t)(index - (WORD_BREAK_DATA_COUNT * 2));
  }
  // If we made it out here, then our scalar was not found in the word
  // array (this occurs when a scalar doesn't map to any word break
  // property). Return the max value here to indicate .any.
  return UINT8_MAX;
#endif
}

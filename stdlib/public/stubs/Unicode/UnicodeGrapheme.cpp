//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#if SWIFT_STDLIB_ENABLE_UNICODE_DATA
#include "Common/GraphemeData.h"
#else
#include "swift/Runtime/Debug.h"
#endif
#include "swift/shims/UnicodeData.h"
#include <stdint.h>


SWIFT_RUNTIME_STDLIB_INTERNAL
__swift_uint8_t _swift_stdlib_getGraphemeBreakProperty(__swift_uint32_t scalar) {
#if !SWIFT_STDLIB_ENABLE_UNICODE_DATA
  swift::swift_abortDisabledUnicodeSupport();
#else
  auto index = 1; //0th element is a dummy element
  while (index < GRAPHEME_BREAK_DATA_COUNT) {
    auto entry = _swift_stdlib_graphemeBreakProperties[index];
    
    // Shift the enum and range count out of the value.
    auto lower = (entry << 11) >> 11;
    
    // Shift the enum out first, then shift out the scalar value.
    auto upper = lower + ((entry << 3) >> 24);
    
    // Shift everything out.
    auto enumValue = (__swift_uint8_t)(entry >> 29);
    
    // Special case: extendedPictographic who used an extra bit for the range.
    if (enumValue == 5) {
      upper = lower + ((entry << 2) >> 23);
    }
    
    //If we want the left child of the current node in our virtual tree,
    //that's at index * 2, if we want the right child it's at (index * 2) + 1
    index = scalar < lower ?
      2 * index :
      scalar <= upper ?
        (GRAPHEME_BREAK_DATA_COUNT * 2) + enumValue :
        2 * index + 1;
  }
  if (index >= GRAPHEME_BREAK_DATA_COUNT * 2) {
    return (__swift_uint8_t)(index - (GRAPHEME_BREAK_DATA_COUNT * 2));
  }
  // If we made it out here, then our scalar was not found in the grapheme
  // array (this occurs when a scalar doesn't map to any grapheme break
  // property). Return the max value here to indicate .any.
  return 0xFF;
#endif
}

SWIFT_RUNTIME_STDLIB_INTERNAL
__swift_bool _swift_stdlib_isLinkingConsonant(__swift_uint32_t scalar) {
#if !SWIFT_STDLIB_ENABLE_UNICODE_DATA
  swift::swift_abortDisabledUnicodeSupport();
#else
  auto idx = _swift_stdlib_getScalarBitArrayIdx(scalar,
                                          _swift_stdlib_linkingConsonant,
                                          _swift_stdlib_linkingConsonant_ranks);

  if (idx == INTPTR_MAX) {
    return false;
  }

  return true;
#endif
}

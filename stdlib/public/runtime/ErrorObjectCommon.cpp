//===--- ErrorObjectCommon.cpp - Recoverable error object -----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This implements the parts of the standard Error protocol type which are
// shared between the ObjC-interoperable implementation and the native
// implementation. The parts specific to each implementation can be found in
// ErrorObject.mm (for the ObjC-interoperable parts) and ErrorObjectNative.cpp.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/Concurrent.h"
#include "swift/Runtime/Config.h"
#include "ErrorObject.h"
#include "ErrorObjectTestSupport.h"

using namespace swift;

void (*swift::_swift_willThrow)(SwiftError *error);

/// Breakpoint hook for debuggers, and calls _swift_willThrow if set.
SWIFT_CC
void swift::swift_willThrow(SWIFT_CONTEXT void *unused,
                            SWIFT_ERROR_RESULT SwiftError **error) {
  // Cheap check to bail out early, since we expect there to be no callbacks
  // the vast majority of the time.
  if (SWIFT_LIKELY(!_swift_willThrow))
    return;
  _swift_willThrow(*error);
}

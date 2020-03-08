//===----------------------------------------------------------------------===//
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

#include "swift/Runtime/Config.h"

#if SWIFT_OBJC_INTEROP
#import <objc/runtime.h>
#import <objc/NSObject.h>

SWIFT_CC SWIFT_RUNTIME_STDLIB_API
bool _swift_stdlib_NSObject_isKindOfClass(
    id _Nonnull object,
    char * _Nonnull className) {
  Class cls = objc_lookUpClass(className);
  return [object isKindOfClass:cls];
}
#endif


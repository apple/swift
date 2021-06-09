//===--- BridgedSwiftObject.h - C header which defines SwiftObject --------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This is a C header, which defines the SwiftObject header. For the C++ version
// see SwiftObjectHeader.h.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_BRIDGEDSWIFTOBJECT_H
#define SWIFT_SIL_BRIDGEDSWIFTOBJECT_H

#include <stdint.h>

#if !__has_feature(nullability)
# define _Nullable
# define _Nonnull
# define _Null_unspecified
#endif

typedef const void * _Nonnull SwiftMetatype;

/// The header of a Swift object.
///
/// This must be in sync with HeapObject, which is defined in the runtime lib.
struct BridgedSwiftObject {
  SwiftMetatype metatype;
  int64_t refCounts;
};

typedef struct BridgedSwiftObject * _Nonnull SwiftObject;
typedef struct BridgedSwiftObject * _Nullable OptionalSwiftObject;

#endif

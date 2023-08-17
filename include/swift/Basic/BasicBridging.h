//===--- BasicBridging.h - header for the swift BasicBridging module ------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_BASICBRIDGING_H
#define SWIFT_BASIC_BASICBRIDGING_H

// Workaround to avoid a compiler error because `cas::ObjectRef` is not defined
// when including VirtualFileSystem.h
#include <cassert>
#include "llvm/CAS/CASReference.h"

#include "swift/Basic/BridgedSwiftObject.h"
#include "swift/Basic/Nullability.h"
#include "swift/Basic/SourceLoc.h"
#include <stddef.h>

SWIFT_BEGIN_NULLABILITY_ANNOTATIONS

typedef intptr_t SwiftInt;
typedef uintptr_t SwiftUInt;

typedef struct {
  const void * _Nullable data;
  size_t numElements;
} BridgedArrayRef;

typedef struct {
  void * _Nonnull streamAddr;
} BridgedOStream;

void OStream_write(BridgedOStream os, llvm::StringRef str);

SWIFT_END_NULLABILITY_ANNOTATIONS

#endif

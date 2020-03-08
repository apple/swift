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
#include "swift/Runtime/Metadata.h"

using namespace swift;

SWIFT_CC LLVM_LIBRARY_VISIBILITY extern "C"
const char *getMetadataKindOf(
    OpaqueValue *value,
    const Metadata *type
) {
  switch (type->getKind()) {
#define METADATAKIND(NAME, VALUE) \
  case MetadataKind::NAME: return #NAME;
#include "swift/ABI/MetadataKind.def"

  default: return "none of your business";
  }
}


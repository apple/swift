//===--- SwiftObjectHeader.h - Defines SwiftObjectHeader ------------------===//
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
#ifndef SWIFT_SIL_SWIFTOBJECTHEADER_H
#define SWIFT_SIL_SWIFTOBJECTHEADER_H

#include "BridgedSwiftObject.h"

/// The C++ version of SwiftObject.
///
/// In C++ code, never use BridgedSwiftObject directly. SwiftObjectHeader has
/// the proper constructor, which avoids the header to be uninitialized.
struct SwiftObjectHeader : BridgedSwiftObject {
  SwiftObjectHeader(SwiftMetatype metatype) {
     this->metatype = metatype;
     this->refCounts = ~(uint64_t)0;
  }
  
  bool isBridged() const {
    return metatype != nullptr;
  }
};

#endif

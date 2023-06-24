//===--- GenericMetadataCacheEntry.h ----------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Declares a struct that mirrors the layout of GenericCacheEntry in
// Metadata.cpp and use a static assert to check that the offset of
// the member Value match between the two.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_REFLECTION_GENERICMETADATACACHEENTRY_H
#define SWIFT_REFLECTION_GENERICMETADATACACHEENTRY_H

#include <cstdint>

namespace swift {

template<typename StoredPointer>
struct GenericMetadataCacheEntry {
  StoredPointer TrackingInfo;
  uint16_t NumKeyParameters;
  uint16_t NumWitnessTables;
  uint16_t NumPacks;
  uint16_t NumShapeClasses;
  StoredPointer PackShapeDescriptors;
  uint32_t Hash;
  StoredPointer Value;
};

} // namespace swift

#endif // SWIFT_REFLECTION_GENERICMETADATACACHEENTRY_H

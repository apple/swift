//===--- ImageInspection.h - Image inspection routines ----------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// This file includes routines that extract metadata from executable and
/// dynamic library image files generated by the Swift compiler. The concrete
/// implementations vary greatly by platform.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_RUNTIME_IMAGEINSPECTION_H
#define SWIFT_RUNTIME_IMAGEINSPECTION_H

#include "ImageInspectionELF.h"
#include <cstdint>
#include <cstddef>

namespace swift {

/// This is a platform independent version of Dl_info from dlfcn.h
struct SymbolInfo {
  const char *fileName;
  void *baseAddress;
  const char *symbolName;
  void *symbolAddress;
};

/// Load the metadata from the image necessary to find protocols by name.
void initializeProtocolLookup();

/// Load the metadata from the image necessary to find a type's
/// protocol conformance.
void initializeProtocolConformanceLookup();

/// Load the metadata from the image necessary to find a type by name.
void initializeTypeMetadataRecordLookup();

// Callbacks to register metadata from an image to the runtime.
void addImageProtocolsBlockCallback(const void *start, uintptr_t size);
void addImageProtocolConformanceBlockCallback(const void *start,
                                              uintptr_t size);
void addImageTypeMetadataRecordBlockCallback(const void *start,
                                             uintptr_t size);

int lookupSymbol(const void *address, SymbolInfo *info);
void *lookupSection(const char *segment, const char *section, size_t *outSize);

} // end namespace swift

#endif

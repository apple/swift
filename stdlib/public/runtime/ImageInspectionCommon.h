//===--- ImageInspectionCommon.h - Image inspection routines -----*- C++ -*-===//
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
///
/// \file
///
/// This file unifies common ELF and COFF image inspection routines.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_RUNTIME_IMAGEINSPECTIONCOMMON_H
#define SWIFT_RUNTIME_IMAGEINSPECTIONCOMMON_H

#if defined(__MACH__)

#include <mach-o/dyld.h>

/// The Mach-O section name for the section containing protocol descriptor
/// references. This lives within SEG_TEXT.
#define MachOProtocolsSection "__swift5_protos"
/// The Mach-O section name for the section containing protocol conformances.
/// This lives within SEG_TEXT.
#define MachOProtocolConformancesSection "__swift5_proto"
/// The Mach-O section name for the section containing type references.
/// This lives within SEG_TEXT.
#define MachOTypeMetadataRecordSection "__swift5_types"
/// The Mach-O section name for the section containing dynamic replacements.
/// This lives within SEG_TEXT.
#define MachODynamicReplacementSection "__swift5_replace"
#define MachODynamicReplacementSomeSection "__swift5_replac2"

#define MachOTextSegment "__TEXT"

#else

#if defined(__ELF__) || defined(__wasm__)
#define SWIFT_REFLECTION_METADATA_ELF_NOTE_MAGIC_STRING "swift_reflection_metadata_magic_string"
#endif // defined(__ELF__)

#include "../SwiftShims/Visibility.h"
#include <cstdint>
#include <cstddef>

namespace swift {
struct MetadataSections;
static constexpr const uintptr_t CurrentSectionMetadataVersion = 1;
}

struct SectionInfo {
  uint64_t size;
  const char *data;
};

// Called by injected constructors when a dynamic library is loaded.
SWIFT_RUNTIME_EXPORT
void swift_addNewDSOImage(const void *addr);

#ifndef NDEBUG

SWIFT_RUNTIME_EXPORT
const char *swift_getMetadataSectionName(void *metadata_section);

SWIFT_RUNTIME_EXPORT
size_t swift_getMetadataSectionCount();

#endif // NDEBUG

#endif // !defined(__MACH__)

#endif // SWIFT_RUNTIME_IMAGEINSPECTIONCOMMON_H

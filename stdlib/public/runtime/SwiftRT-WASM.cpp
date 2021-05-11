//===--- SwiftRT-WASM.cpp --------------------------------------------------===//
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

#include "ImageInspectionCommon.h"
#include "../SwiftShims/MetadataSections.h"

#include <cstddef>

// Link start/stop symbols weakly to link them if they aren't synthesized by the linker. 
#define DECLARE_SWIFT_SECTION(name)                                                             \
  __attribute__((__visibility__("hidden"),__aligned__(1),weak)) extern const char __start_##name;    \
  __attribute__((__visibility__("hidden"),__aligned__(1),weak)) extern const char __stop_##name;

extern "C" {
DECLARE_SWIFT_SECTION(swift5_protocols)
DECLARE_SWIFT_SECTION(swift5_protocol_conformances)
DECLARE_SWIFT_SECTION(swift5_type_metadata)

DECLARE_SWIFT_SECTION(swift5_typeref)
DECLARE_SWIFT_SECTION(swift5_reflstr)
DECLARE_SWIFT_SECTION(swift5_fieldmd)
DECLARE_SWIFT_SECTION(swift5_assocty)
DECLARE_SWIFT_SECTION(swift5_replace)
DECLARE_SWIFT_SECTION(swift5_replac2)
DECLARE_SWIFT_SECTION(swift5_builtin)
DECLARE_SWIFT_SECTION(swift5_capture)
}

#undef DECLARE_SWIFT_SECTION

namespace {
static swift::MetadataSections sections{};
}

__attribute__((__constructor__))
static void swift_image_constructor() {
#define SWIFT_SECTION_RANGE(name)                                              \
  { reinterpret_cast<uintptr_t>(&__start_##name),                              \
    static_cast<uintptr_t>(&__stop_##name - &__start_##name) }

  sections = {
      swift::CurrentSectionMetadataVersion,
      0,

      nullptr,
      nullptr,

      SWIFT_SECTION_RANGE(swift5_protocols),
      SWIFT_SECTION_RANGE(swift5_protocol_conformances),
      SWIFT_SECTION_RANGE(swift5_type_metadata),

      SWIFT_SECTION_RANGE(swift5_typeref),
      SWIFT_SECTION_RANGE(swift5_reflstr),
      SWIFT_SECTION_RANGE(swift5_fieldmd),
      SWIFT_SECTION_RANGE(swift5_assocty),
      SWIFT_SECTION_RANGE(swift5_replace),
      SWIFT_SECTION_RANGE(swift5_replac2),
      SWIFT_SECTION_RANGE(swift5_builtin),
      SWIFT_SECTION_RANGE(swift5_capture),
  };

#undef SWIFT_SECTION_RANGE

  swift_addNewDSOImage(&sections);
}

static __attribute__((__used__))
__attribute__((__section__(".note.swift_reflection_metadata")))
__attribute__((__aligned__(1)))
struct {
  const char MagicString[sizeof(SWIFT_REFLECTION_METADATA_ELF_NOTE_MAGIC_STRING)];
  const swift::MetadataSections *Sections;
} __attribute__((__packed__))
Note = {SWIFT_REFLECTION_METADATA_ELF_NOTE_MAGIC_STRING, &sections};

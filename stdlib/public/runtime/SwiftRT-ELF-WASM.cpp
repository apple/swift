//===--- SwiftRT-ELF-WASM.cpp ---------------------------------------------===//
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
#include "swift/shims/MetadataSections.h"

#include <cstddef>
#include <new>

#if defined(__ELF__)
extern "C" const char __dso_handle[];
#elif defined(__wasm__)
// NOTE: Multi images in a single process is not yet
// stabilized in WebAssembly toolchain outside of Emscripten.
static constexpr const void *__dso_handle = nullptr;
#endif

// Create empty sections to ensure that the start/stop symbols are synthesized
// by the linker.  Otherwise, we may end up with undefined symbol references as
// the linker table section was never constructed.
#if defined(__ELF__)
# define DECLARE_EMPTY_METADATA_SECTION(name) __asm__("\t.section " #name ",\"a\"\n");
#elif defined(__wasm__)
# define DECLARE_EMPTY_METADATA_SECTION(name) __asm__("\t.section " #name ",\"\",@\n");
#endif

#define DECLARE_SWIFT_SECTION(name)                                                          \
  DECLARE_EMPTY_METADATA_SECTION(name)                                                                \
  __attribute__((__visibility__("hidden"),__aligned__(1))) extern const char __start_##name; \
  __attribute__((__visibility__("hidden"),__aligned__(1))) extern const char __stop_##name;

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
DECLARE_SWIFT_SECTION(swift5_mpenum)
DECLARE_SWIFT_SECTION(swift5_accessible_functions)
DECLARE_SWIFT_SECTION(swift5_runtime_attributes)
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

  ::new (&sections) swift::MetadataSections {
      swift::CurrentSectionMetadataVersion,
      { __dso_handle },

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
      SWIFT_SECTION_RANGE(swift5_mpenum),
      SWIFT_SECTION_RANGE(swift5_accessible_functions),
      SWIFT_SECTION_RANGE(swift5_runtime_attributes),
  };

#undef SWIFT_SECTION_RANGE

  swift_addNewDSOImage(&sections);
}
